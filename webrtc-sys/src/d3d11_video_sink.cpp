/* Copyright 2025 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "livekit/d3d11_video_sink.h"

#if defined(_WIN32)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "api/video/video_frame_buffer.h"
#include "api/video/i420_buffer.h"
#include "libyuv/convert_argb.h"
#include "libyuv/scale.h"

namespace livekit_ffi {
namespace {

using Microsoft::WRL::ComPtr;

D3D_DRIVER_TYPE DriverCandidates[] = {
    D3D_DRIVER_TYPE_HARDWARE,
    D3D_DRIVER_TYPE_WARP,
};

// 消费端不在场（窗口隐藏 / 当前版面不显示这块纹理）时，拿不到 keyed mutex 不重试，先退避这么久。
// 目的：既不让 WebRTC 的视频线程卡死，也不做无退避的重试热循环。
constexpr double kConsumerAbsentBackoffMs = 200.0;
constexpr DWORD kAcquireTimeoutMs = 50;

// 帧计数共享内存（frame_view_）的布局：LONG 数组，C# 侧读它做 [GpuPresent] 诊断。
//   0 = 已写成的帧号（消费端只在它变化时才去动合成 surface）
//   1 = producer_active（消费端在不在场：0 = 没人看，原生连锁都不抢）
//   2 = 限速丢弃：被 max_fps 挡在**转换之前**的帧数（"到了但没转"的那部分）
//   3 = 消费端不在场丢弃：producer_active=0 或退避期丢掉的帧数
// 于是 C# 侧可以算出一个"到 sink 的帧数 ≈ 写入 + 限速丢 + 无宿主丢"，
// 一眼看出"远端推了多少、本端只转了多少"（VDI 上最关心的那笔账）。
constexpr int kFrameSlotCount = 5;
constexpr int kFrameIdSlot = 0;
constexpr int kProducerActiveSlot = 1;
constexpr int kRateLimitedDropSlot = 2;
constexpr int kConsumerAbsentDropSlot = 3;
// 4 = 生产端"把钥匙交还给消费端"的次数（诊断用：这条在涨 = 生产端一直在替消费端解套；
// 为 0 而消费端的合成更新又等很久 = 钥匙真的被谁按住了）。
constexpr int kTokenHandBackSlot = 4;

// 销毁路径上"有界等消费端放手"的时间。见 HandBackConsumerKeyLocked 的说明：
// 消费端那次 acquire 是**无限等**，所以销毁前必须把钥匙交还；但也不能为了等它而无限阻塞销毁。
constexpr uint32_t kTeardownHandBackWaitMs = 100;

/// 拿到 keyed mutex（key 0）之后的 RAII 守卫。
///
/// 协议是"生产端 acquire(0) → 写 → release(1)；消费端 acquire(1) → 读 → release(0)"的**严格轮转**，
/// 而消费端等的是 `AcquireSync(key, int.MaxValue)`（Avalonia 的
/// `AngleExternalMemoryD3D11Texture2D::AcquireKeyedMutex`，合成/渲染线程上）。也就是说：
/// 钥匙只要漏在生产端手里，那个线程就**永远**不返回 —— 整个窗口不再重绘
/// （现场：`一次合成更新耗时 4094 ms`、界面僵住、紧接着"消费端还在用、原生在退役"直接崩进程）。
///
/// 所以"拿到钥匙之后"的每一步都不允许提前 return：这个守卫保证任何出口都会 release(1)。
struct KeyedMutexLease {
  IDXGIKeyedMutex* mutex = nullptr;

  explicit KeyedMutexLease(IDXGIKeyedMutex* m) : mutex(m) {}
  ~KeyedMutexLease() {
    if (mutex != nullptr) {
      mutex->ReleaseSync(1);
    }
  }

  KeyedMutexLease(const KeyedMutexLease&) = delete;
  KeyedMutexLease& operator=(const KeyedMutexLease&) = delete;
  KeyedMutexLease(KeyedMutexLease&&) = delete;
  KeyedMutexLease& operator=(KeyedMutexLease&&) = delete;
};

double now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

D3D11VideoSink::D3D11VideoSink(uint32_t width, uint32_t height,
                               uint32_t max_fps)
    : width_(std::max<uint32_t>(2, width & ~1u)),
      height_(std::max<uint32_t>(2, height & ~1u)),
      max_fps_(max_fps),
      min_interval_ms_(
          max_fps > 0 ? 1000.0 / static_cast<double>(max_fps) : 0.0),
      frame_mutex_(new std::mutex()),
      scale_y_(new std::vector<uint8_t>()),
      scale_u_(new std::vector<uint8_t>()),
      scale_v_(new std::vector<uint8_t>()),
      bgra_(new std::vector<uint8_t>()) {
  auto* device = new ComPtr<ID3D11Device>();
  auto* context = new ComPtr<ID3D11DeviceContext>();
  HRESULT last_error = E_FAIL;

  for (auto driver : DriverCandidates) {
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected_level{};
    last_error = D3D11CreateDevice(
        nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        device->GetAddressOf(), &selected_level, context->GetAddressOf());
    if (SUCCEEDED(last_error)) {
      break;
    }
    device->Reset();
    context->Reset();
  }

  if (FAILED(last_error) || !*device || !*context) {
    delete device;
    delete context;
    ReleaseResources();
    return;
  }

  device_ = device;
  context_ = context;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width_;
  desc.Height = height_;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

  auto* texture = new ComPtr<ID3D11Texture2D>();
  HRESULT hr = (*device)->CreateTexture2D(&desc, nullptr, texture->GetAddressOf());
  if (FAILED(hr) || !*texture) {
    delete texture;
    ReleaseResources();
    return;
  }
  texture_ = texture;

  auto* mutex = new ComPtr<IDXGIKeyedMutex>();
  hr = (*texture)->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(mutex->GetAddressOf()));
  if (FAILED(hr) || !*mutex) {
    delete mutex;
    ReleaseResources();
    return;
  }
  keyed_mutex_ = mutex;

  ComPtr<IDXGIResource> resource;
  hr = (*texture)->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(resource.GetAddressOf()));
  if (FAILED(hr) || !resource) {
    ReleaseResources();
    return;
  }

  HANDLE shared_handle = nullptr;
  hr = resource->GetSharedHandle(&shared_handle);
  if (FAILED(hr) || shared_handle == nullptr) {
    ReleaseResources();
    return;
  }

  shared_handle_ = reinterpret_cast<uint64_t>(shared_handle);

  wchar_t frame_mapping_name[64]{};
  swprintf_s(frame_mapping_name, L"Local\\LiveKitGpuFrame_%016llX",
             static_cast<unsigned long long>(shared_handle_));
  frame_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0,
                                      sizeof(LONG) * kFrameSlotCount,
                                      frame_mapping_name);
  if (frame_mapping_ != nullptr) {
    frame_view_ = MapViewOfFile(frame_mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                sizeof(LONG) * kFrameSlotCount);
    if (frame_view_ != nullptr) {
      auto* slots = static_cast<volatile LONG*>(frame_view_);
      slots[kFrameIdSlot] = 0;
      slots[kProducerActiveSlot] = 1;
      slots[kRateLimitedDropSlot] = 0;
      slots[kConsumerAbsentDropSlot] = 0;
      slots[kTokenHandBackSlot] = 0;
    }
  }

  // Prime the keyed mutex with a black frame. Avalonia's first
  // UpdateWithKeyedMutexAsync(acquire=1, release=0) can then complete
  // immediately instead of waiting for the first decoded frame.
  // （新建的 keyed mutex 初始就允许 acquire(0)，这里给个 1 秒上限只是不写 INFINITE。）
  auto& initial_bgra = *static_cast<std::vector<uint8_t>*>(bgra_);
  initial_bgra.assign(static_cast<size_t>(width_) * height_ * 4, 0);
  if ((*mutex)->AcquireSync(0, 1000) == S_OK) {
    KeyedMutexLease lease((*mutex).Get());
    (*context)->UpdateSubresource(texture->Get(), 0, nullptr,
                                  initial_bgra.data(),
                                  static_cast<UINT>(width_) * 4, 0);
  }

  valid_ = true;
}

D3D11VideoSink::~D3D11VideoSink() {
  ReleaseResources();
  delete static_cast<std::mutex*>(frame_mutex_);
  delete static_cast<std::vector<uint8_t>*>(scale_y_);
  delete static_cast<std::vector<uint8_t>*>(scale_u_);
  delete static_cast<std::vector<uint8_t>*>(scale_v_);
  delete static_cast<std::vector<uint8_t>*>(bgra_);
}

void D3D11VideoSink::ReleaseResources() {
  // ★ 销毁必须和**生产端自己的写线程**串行。以前这里不取 frame_mutex_，而 OnFrame 可能正拿着
  //   钥匙在做 UpdateSubresource —— 此刻删掉纹理/上下文/设备是标准的 use-after-free
  //   （现场：反复开关摄像头偶发"进程直接消失、日志截断"）。
  std::unique_lock<std::mutex> teardown_lock(*static_cast<std::mutex*>(frame_mutex_));

  // 先把自己关掉：这一行之后 OnFrame 再进来就直接返回，不会再有新的写。
  valid_ = false;

  // 再把钥匙交还给消费端（有界等它先放手）：这样"消费端那次等 key 1"立刻返回，
  // 不会留下"消费端还在等 / 原生已经在退役纹理"的窗口。等不到也不阻塞销毁 ——
  // 纹理还有 10 秒回收宽限（见 retire_d3d11_video_sink），没人引用时才会真被释放。
  HandBackConsumerKeyLocked(kTeardownHandBackWaitMs);

  if (frame_view_ != nullptr) {
    UnmapViewOfFile(frame_view_);
    frame_view_ = nullptr;
  }
  if (frame_mapping_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(frame_mapping_));
    frame_mapping_ = nullptr;
  }
  delete static_cast<ComPtr<IDXGIKeyedMutex>*>(keyed_mutex_);
  delete static_cast<ComPtr<ID3D11Texture2D>*>(texture_);
  delete static_cast<ComPtr<ID3D11DeviceContext>*>(context_);
  delete static_cast<ComPtr<ID3D11Device>*>(device_);
  keyed_mutex_ = nullptr;
  texture_ = nullptr;
  context_ = nullptr;
  device_ = nullptr;
  shared_handle_ = 0;
  valid_ = false;
}

void D3D11VideoSink::set_producer_active(bool active) const {
  // ★ 这个函数**只写标志位，绝不碰 keyed mutex**（2026-10-13 踩过，见 OnFrame 里被按住那一支的注释）：
  //   观看侧"松手"（active=true）之后紧接着就等我们写帧，此刻钥匙必须是"key 0 可用"；
  //   在这里交还钥匙等于把生产端自己的轮次让出去，帧号会永远停在原地。
  if (frame_view_ != nullptr) {
    static_cast<volatile LONG*>(frame_view_)[kProducerActiveSlot] = active ? 1 : 0;
  }
}

void D3D11VideoSink::CountDrop(int slot) const {
  if (frame_view_ != nullptr) {
    InterlockedIncrement(static_cast<volatile LONG*>(frame_view_) + slot);
  }
}

void D3D11VideoSink::retire_consumer_handshake() {
  if (frame_mutex_ == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(*static_cast<std::mutex*>(frame_mutex_));

  // 只做**非阻塞**的唤醒：如果有消费端正等着 key 1，这里把 key 1 交给它。
  //
  // 以前这里会再等一次 `AcquireSync(0, 5000)`，而调用方可能正是 UI 线程 ——
  // 实测那一次等待在最常见的"最后一帧已经显示过、之后没有新帧"的情况下会**等满 5 秒**，
  // 现场表现就是"结束共享的瞬间整个界面停住、点什么都没反应"。
  // 消费端拿到 key 后自己会释放 key 0；纹理本身还有 10 秒的回收宽限，不需要在这里同步等。
  HandBackConsumerKeyLocked(0);
}

void D3D11VideoSink::HandBackConsumerKeyLocked(uint32_t timeout_ms) const {
  auto* mutex = static_cast<ComPtr<IDXGIKeyedMutex>*>(keyed_mutex_);
  if (mutex == nullptr || !*mutex) {
    return;
  }

  // **只允许在"生产端再也不会写帧"的终态调用**（退役 / 销毁 / 根本没有共享内存可写）：
  //   · 只有"无人持有 + 上次释放的是 key 0"这一态才拿得到，而那一态正是**消费端拿不到 key 1** 的态
  //     （它此刻若在等，就是无限等，见 Avalonia 的 AcquireSync(key, int.MaxValue)）；
  //   · 拿到就立刻以 key 1 还给消费端：内容一个字节都没动，它读到的还是最后一帧，属于"合法唤醒"。
  //   · 其余两种情况都是空操作：钥匙已经在消费端手上（它会自己还）/ 已经摆着等它来取。
  // **中途的"这一拍不写"（被按住 / 限速 / 退避）绝不能调**：那会把生产端自己的轮次让出去，
  //   帧号会永远停在原地（2026-10-13 的回归）。
  // timeout_ms = 0 → 非阻塞；销毁路径传几十毫秒 → 先让消费端拷完再交还。
  if ((*mutex)->AcquireSync(0, timeout_ms) != S_OK) {
    return;
  }
  (*mutex)->ReleaseSync(1);

  if (frame_view_ != nullptr) {
    InterlockedIncrement(static_cast<volatile LONG*>(frame_view_) +
                         kTokenHandBackSlot);
  }
}


void D3D11VideoSink::OnFrame(const webrtc::VideoFrame& frame) {
  if (frame_mutex_ == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(*static_cast<std::mutex*>(frame_mutex_));
  if (!valid_) {
    return;
  }
  if (frame_view_ == nullptr) {
    HandBackConsumerKeyLocked(0);
    return;
  }
  if (static_cast<volatile LONG*>(frame_view_)[kProducerActiveSlot] == 0) {
    CountDrop(kConsumerAbsentDropSlot);   // 本端明确表示"没人看这块纹理"
    // ★ 这里**不能**交还钥匙（2026-10-13 踩过）：观看侧按住我们只是为了省掉这一次"转换+上传"，
    //   下一拍它就会松手让我们写。若此刻把钥匙交给消费端，状态变成"key 1 可被取"，
    //   而我们写帧需要的是"key 0 可被取" —— 松手后 AcquireSync(0) 会一直失败，
    //   消费端又只在帧号变化时才合成 ⇒ 帧号永远不动（VDI 现场：帧号停在 2、4.5 秒后判"没有新帧"）。
    return;
  }

  // 消费端不在场时已经退避：直接丢掉这一帧（连 I420 转换都不做），
  // 别把 WebRTC 的视频线程耗在没人看的帧上。
  if (now_ms() < skip_until_ms_) {
    CountDrop(kConsumerAbsentDropSlot);
    return;   // 同上：退避只是"这一拍不写"，钥匙必须留在"key 0 可用"这一态
  }

  // 本端（观看侧）写纹理限速（max_fps>0 时）：还没到下一次允许写纹理的时间 -> 整帧丢掉。
  // **只影响本端显示**：对端照它自己的配置推流（60fps 也照收照解码），这里丢的是"解码之后"的帧。
  // 这一条**必须在转换之前**：VDI 上"每帧 I420->BGRA + 10MB 级上传"才是 CPU 大头，
  // 对端推 30~60fps、本端只显示 15fps 时，剩下的转换与上传全是白烧的 CPU。
  if (min_interval_ms_ > 0 && now_ms() - last_write_ms_ < min_interval_ms_) {
    CountDrop(kRateLimitedDropSlot);
    return;   // 同上：限速丢的这一帧也不许交还钥匙
  }

  auto* mutex = static_cast<ComPtr<IDXGIKeyedMutex>*>(keyed_mutex_);
  auto* context = static_cast<ComPtr<ID3D11DeviceContext>*>(context_);
  auto* texture = static_cast<ComPtr<ID3D11Texture2D>*>(texture_);
  if (mutex == nullptr || !*mutex || context == nullptr || texture == nullptr) {
    return;
  }

  auto buffer = frame.video_frame_buffer();
  if (!buffer) {
    return;
  }

  auto i420 = buffer->ToI420();
  if (!i420) {
    return;
  }

  const int src_width = i420->width();
  const int src_height = i420->height();
  if (src_width <= 0 || src_height <= 0) {
    return;
  }

  const double scale = std::min(
      static_cast<double>(width_) / src_width,
      static_cast<double>(height_) / src_height);
  const int dst_width = std::max(2, static_cast<int>(src_width * scale) & ~1);
  const int dst_height = std::max(2, static_cast<int>(src_height * scale) & ~1);
  const int dst_x = (static_cast<int>(width_) - dst_width) / 2;
  const int dst_y = (static_cast<int>(height_) - dst_height) / 2;

  auto& bgra = *static_cast<std::vector<uint8_t>*>(bgra_);

  // 逐帧热路径上只做必要的工作（VDI 4 核 + 软渲染时这里直接决定 CPU 占用）：
  //   · 源尺寸 == 纹理尺寸（现在的常见情况：纹理就是按源尺寸建的）→ 跳过 I420Scale 这一次全帧拷贝；
  //   · 画面占满整块纹理 → 跳过"铺黑边"的一次全纹理清零；
  //   · 上下翻转交给 libyuv 的**负 stride**（自下而上写）完成，省掉原来那次 11MB 的整帧行交换。
  const bool needs_scale = dst_width != src_width || dst_height != src_height;
  // 缩放核：**缩小**（源比纹理大）时用 box（面积平均）—— 它是"降采样"的正确核，
  // 文字边缘不会像双线性那样出现明暗闪烁的锯齿；1:1 或放大时用线性（更锐）。
  // 阈值取 2.0：只有真正的降采样（例如竖屏 1156x2510 → 496x1080 是 2.3x）才换核，
  // 轻微缩小（如 1080p → 720 的 1.5x）仍保持原来的观感。
  const double downscale_ratio =
      static_cast<double>(src_width) / static_cast<double>(dst_width);
  const libyuv::FilterMode scale_filter =
      downscale_ratio >= 2.0 ? libyuv::kFilterBox : libyuv::kFilterLinear;
  const bool needs_clear = dst_width != static_cast<int>(width_) ||
                           dst_height != static_cast<int>(height_);

  const uint8_t* plane_y = i420->DataY();
  const uint8_t* plane_u = i420->DataU();
  const uint8_t* plane_v = i420->DataV();
  int plane_y_stride = i420->StrideY();
  int plane_chroma_stride = i420->StrideU();
  int plane_width = src_width;
  int plane_height = src_height;

  auto& scaled_y = *static_cast<std::vector<uint8_t>*>(scale_y_);
  auto& scaled_u = *static_cast<std::vector<uint8_t>*>(scale_u_);
  auto& scaled_v = *static_cast<std::vector<uint8_t>*>(scale_v_);
  const int dst_chroma_stride = (dst_width + 1) / 2;

  if (needs_scale) {
    scaled_y.resize(static_cast<size_t>(dst_width) * dst_height);
    scaled_u.resize(static_cast<size_t>(dst_chroma_stride) * ((dst_height + 1) / 2));
    scaled_v.resize(static_cast<size_t>(dst_chroma_stride) * ((dst_height + 1) / 2));

    libyuv::I420Scale(
        i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
        i420->DataV(), i420->StrideV(), src_width, src_height, scaled_y.data(),
        dst_width, scaled_u.data(), dst_chroma_stride, scaled_v.data(),
        dst_chroma_stride, dst_width, dst_height, scale_filter);

    plane_y = scaled_y.data();
    plane_u = scaled_u.data();
    plane_v = scaled_v.data();
    plane_y_stride = dst_width;
    plane_chroma_stride = dst_chroma_stride;
    plane_width = dst_width;
    plane_height = dst_height;
  }

  bgra.resize(static_cast<size_t>(width_) * height_ * 4);
  if (needs_clear) {
    std::fill(bgra.begin(), bgra.end(), 0);   // 只在留黑边时才需要
  }

  // ANGLE samples GL textures with a bottom-left origin, while D3D11 and libyuv store rows
  // top-down, so the frame has to be flipped vertically once. libyuv 的**负 height** 就是官方的
  // 上下翻转用法（内部把目标指针挪到最后一行并把 stride 取负），这样不用事后再遍历一遍
  // 11MB 的行交换（VDI 上那一次遍历是实打实的 CPU）。
  uint8_t* dst_first_row =
      bgra.data() + (static_cast<size_t>(dst_y) * width_ + dst_x) * 4;
  libyuv::I420ToARGB(
      plane_y, plane_y_stride, plane_u, plane_chroma_stride,
      plane_v, plane_chroma_stride, dst_first_row,
      static_cast<int>(width_) * 4, plane_width, -plane_height);

  // 转换做完才抢 key：keyed mutex 只覆盖"上传"这一段，消费端（合成线程）等的时间才短。
  // 这里也**不能**用 INFINITE —— 消费端消失时那会把 WebRTC 的视频线程永久卡住
  // （连带整条轨道的投递都停在那一帧上）。
  if ((*mutex)->AcquireSync(0, kAcquireTimeoutMs) != S_OK) {
    skip_until_ms_ = now_ms() + kConsumerAbsentBackoffMs;
    return;
  }
  // ★★ 这里**必须**用 `== S_OK` 判，不能写 FAILED(...)：AcquireSync 超时返回的是 WAIT_TIMEOUT(0x102)，
  //    它的最高位是 0，`FAILED()` 会判成 false —— 于是"没抢到锁"也会往下走：在消费端**正拷贝这块纹理**
  //    的时候 UpdateSubresource（撕裂/花屏），再 ReleaseSync(1) 被 DXGI 直接拒掉（DXGI_ERROR_INVALID_CALL，
  //    状态不变），帧号却照样 +1 → 消费端认到"有新帧"去合成，而钥匙其实还在它自己那侧 →
  //    它那次 `AcquireSync(1, int.MaxValue)` 就无限等（现场：`一次合成更新耗时 4094 ms`、界面僵住、随后崩）。
  //    2026-10-13 用探针把 keyed mutex 的状态机逐条测了出来才确认这一点：
  //    空闲时 `acquire(0,0)=WAIT_TIMEOUT / acquire(1,0)=S_OK`（钥匙在消费端那侧）；
  //    消费端 release(0) 之后反过来；未持有就 release 会得到 DXGI_ERROR_INVALID_CALL。
  //    回归用例外层已覆盖（producer_hands_back_key_when_it_skips_writing 等）。
  // ★ 钥匙已经到手：从这里开始**任何**出口都必须走 release(1)（守卫的就是那个"漏钥匙"的崩点）。
  {
    KeyedMutexLease lease((*mutex).Get());
    (*context)->UpdateSubresource(texture->Get(), 0, nullptr, bgra.data(),
                                  static_cast<UINT>(width_) * 4, 0);
  }
  last_write_ms_ = now_ms();
  if (frame_view_ != nullptr) {
    InterlockedIncrement(static_cast<volatile LONG*>(frame_view_) + kFrameIdSlot);
  }
}



void D3D11VideoSink::OnDiscardedFrame() {}

// ---------------- 冒烟测试支持 ----------------
// 见头文件里的说明：这些函数只在 webrtc-sys 的单元测试里用，正式路径不调用。

void test_feed_split_frame(const std::shared_ptr<D3D11VideoSink>& sink,
                           uint32_t width, uint32_t height) {
  if (!sink || width < 4 || height < 4) {
    return;
  }

  const int w = static_cast<int>(width);
  const int h = static_cast<int>(height);
  const int half = h / 2;

  // BT.601 纯红：Y=76 / U=84 / V=255；纯蓝：Y=29 / U=255 / V=107。
  // 上半红、下半蓝 —— 只要看纹理第 0 行与最后一行的颜色就能判断上下方向对不对。
  webrtc::scoped_refptr<webrtc::I420Buffer> buffer = webrtc::I420Buffer::Create(w, h);
  for (int y = 0; y < h; ++y) {
    std::memset(buffer->MutableDataY() + static_cast<size_t>(y) * buffer->StrideY(),
                y < half ? 76 : 29, static_cast<size_t>(w));
  }
  for (int y = 0; y < (h + 1) / 2; ++y) {
    std::memset(buffer->MutableDataU() + static_cast<size_t>(y) * buffer->StrideU(),
                y < half / 2 ? 84 : 255, static_cast<size_t>((w + 1) / 2));
    std::memset(buffer->MutableDataV() + static_cast<size_t>(y) * buffer->StrideV(),
                y < half / 2 ? 255 : 107, static_cast<size_t>((w + 1) / 2));
  }

  webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
                                 .set_video_frame_buffer(buffer)
                                 .set_timestamp_us(0)
                                 .build();
  sink->OnFrame(frame);
}

bool test_consume_once(const std::shared_ptr<D3D11VideoSink>& sink) {
  if (!sink) {
    return false;
  }
  auto* mutex = static_cast<ComPtr<IDXGIKeyedMutex>*>(sink->keyed_mutex_);
  if (mutex == nullptr || !*mutex) {
    return false;
  }
  if ((*mutex)->AcquireSync(1, 1000) != S_OK) {
    return false;
  }
  (*mutex)->ReleaseSync(0);
  return true;
}

bool test_acquire_consumer_key(const std::shared_ptr<D3D11VideoSink>& sink) {
  return test_acquire_consumer_key_for(sink, 0);
}

bool test_acquire_consumer_key_for(const std::shared_ptr<D3D11VideoSink>& sink,
                                   uint32_t timeout_ms) {
  if (!sink) {
    return false;
  }
  auto* mutex = static_cast<ComPtr<IDXGIKeyedMutex>*>(sink->keyed_mutex_);
  if (mutex == nullptr || !*mutex) {
    return false;
  }
  // 语义与消费端一致（Avalonia 是 int.MaxValue 无限等，这里给个上限好让用例能失败退出）：
  // 拿不到 key 1 就是"消费端那一等不会返回"。
  // 注意这里**不能**写 SUCCEEDED(...)：AcquireSync 超时返回 WAIT_TIMEOUT(0x102)，它的最高位是 0，
  // SUCCEEDED 会返回 true —— 那等于"超时也算拿到了"，这个用例就永远测不出问题。
  return (*mutex)->AcquireSync(1, timeout_ms) == S_OK;
}

void test_release_consumer_key(const std::shared_ptr<D3D11VideoSink>& sink) {
  if (!sink) {
    return;
  }
  auto* mutex = static_cast<ComPtr<IDXGIKeyedMutex>*>(sink->keyed_mutex_);
  if (mutex == nullptr || !*mutex) {
    return;
  }
  (*mutex)->ReleaseSync(0);
}

bool test_read_texture(const std::shared_ptr<D3D11VideoSink>& sink,
                       uint32_t& top_left_pixel,
                       uint32_t& bottom_left_pixel) {
  if (!sink || !sink->valid_) {
    return false;
  }

  auto* device = static_cast<ComPtr<ID3D11Device>*>(sink->device_);
  auto* context = static_cast<ComPtr<ID3D11DeviceContext>*>(sink->context_);
  auto* texture = static_cast<ComPtr<ID3D11Texture2D>*>(sink->texture_);
  auto* mutex = static_cast<ComPtr<IDXGIKeyedMutex>*>(sink->keyed_mutex_);
  if (device == nullptr || context == nullptr || texture == nullptr ||
      mutex == nullptr || !*mutex) {
    return false;
  }

  if ((*mutex)->AcquireSync(1, 1000) != S_OK) {
    return false;
  }

  bool ok = false;
  D3D11_TEXTURE2D_DESC desc{};
  (*texture)->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;

  ComPtr<ID3D11Texture2D> staging;
  if (SUCCEEDED((*device)->CreateTexture2D(&desc, nullptr,
                                          staging.GetAddressOf()))) {
    (*context)->CopyResource(staging.Get(), (*texture).Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(
            (*context)->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
      top_left_pixel = *reinterpret_cast<const uint32_t*>(bytes);
      bottom_left_pixel = *reinterpret_cast<const uint32_t*>(
          bytes + static_cast<size_t>(desc.Height - 1) * mapped.RowPitch);
      (*context)->Unmap(staging.Get(), 0);
      ok = true;
    }
  }

  (*mutex)->ReleaseSync(0);
  return ok;
}

int32_t test_handback_count(const std::shared_ptr<D3D11VideoSink>& sink) {
  if (!sink || sink->frame_view_ == nullptr) {
    return -1;
  }
  return static_cast<volatile LONG*>(sink->frame_view_)[kTokenHandBackSlot];
}

int32_t test_frame_counter(const std::shared_ptr<D3D11VideoSink>& sink) {
  if (!sink || sink->frame_view_ == nullptr) {
    return -1;
  }
  return static_cast<volatile LONG*>(sink->frame_view_)[kFrameIdSlot];
}

bool test_read_drop_counters(const std::shared_ptr<D3D11VideoSink>& sink,
                             int32_t& rate_limited,
                             int32_t& consumer_absent) {
  if (!sink || sink->frame_view_ == nullptr) {
    return false;
  }
  auto* slots = static_cast<volatile LONG*>(sink->frame_view_);
  rate_limited = slots[kRateLimitedDropSlot];
  consumer_absent = slots[kConsumerAbsentDropSlot];
  return true;
}

uint64_t D3D11VideoSink::shared_handle() const { return shared_handle_; }
uint32_t D3D11VideoSink::width() const { return width_; }
uint32_t D3D11VideoSink::height() const { return height_; }
uint32_t D3D11VideoSink::max_fps() const { return max_fps_; }
bool D3D11VideoSink::valid() const { return valid_; }

void retire_d3d11_video_sink(
    const std::shared_ptr<D3D11VideoSink>& sink) {
  if (!sink) {
    return;
  }

  sink->retire_consumer_handshake();
  std::thread([sink]() {
    std::this_thread::sleep_for(std::chrono::seconds(10));
  }).detach();
}


std::shared_ptr<D3D11VideoSink> new_d3d11_video_sink(uint32_t width,
                                                    uint32_t height,
                                                    uint32_t max_fps) {
  return std::make_shared<D3D11VideoSink>(width, height, max_fps);
}

}  // namespace livekit_ffi

#endif  // defined(_WIN32)
