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
// 注意：本文件在**非 Apple 平台上也参与编辑**，但只在 Apple 平台上被编译
// （build.rs / video_track.cpp 里都有 __APPLE__ 守卫）。
#include "livekit/iosurface_video_sink.h"

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>   // kIOReturnSuccess
#include <IOSurface/IOSurface.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame_buffer.h"
#include "libyuv/convert_argb.h"
#include "libyuv/scale.h"

namespace livekit_ffi {
namespace {

double now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch())
      .count();
}

// 消费端不在场（窗口隐藏 / 当前版面不显示这块 surface）时，退避这么久再试，
// 既不让 WebRTC 的视频线程空转，也不做无退避的重试热循环。语义与 Windows 版一致。
constexpr double kConsumerAbsentBackoffMs = 200.0;

// 计数器共享内存的槽位布局（与 Windows 版逐位一致，C# 侧同一套读法）：
//   0 = 已写成的帧号，1 = producer_active，2 = 限速丢弃，3 = 消费端不在场丢弃，
//   5/6 = **解码后的真实帧宽高**（每写成一帧更新）。
//
// 为什么 mac 也要这两个槽：IOSurface 的宽高在 `new_iosurface_video_sink()` 时定死，
// 源换方向（手机竖屏 ↔ 横屏）时这里只能把新画面**等比塞进旧 surface 并居中留黑**，
// 上层再按 surface 比例摆放就是"中间一条小画面"。观看端（GpuTexturePresenter）
// 读 slot 5/6 发现"surface 比例 ≠ 源比例"后会按新尺寸重建 surface。
constexpr int kFrameSlotCount = 7;
constexpr int kFrameIdSlot = 0;
constexpr int kProducerActiveSlot = 1;
constexpr int kRateLimitedDropSlot = 2;
constexpr int kConsumerAbsentDropSlot = 3;
constexpr int kSrcWidthSlot = 5;
constexpr int kSrcHeightSlot = 6;
constexpr int kCounterSize = kFrameSlotCount * 4;

}  // namespace

IOSurfaceVideoSink::IOSurfaceVideoSink(uint32_t width, uint32_t height,
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
  const int32_t w = static_cast<int32_t>(width_);
  const int32_t h = static_cast<int32_t>(height_);
  const int32_t bytes_per_element = 4;
  const int32_t bytes_per_row = w * bytes_per_element;
  const int32_t pixel_format = 'BGRA';  // kCVPixelFormatType_32BGRA

  CFMutableDictionaryRef props = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (props == nullptr) {
    ReleaseResources();
    return;
  }

  auto set_number = [&](CFStringRef key, int32_t value) {
    CFNumberRef number =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    if (number != nullptr) {
      CFDictionarySetValue(props, key, number);
      CFRelease(number);
    }
  };
  set_number(kIOSurfaceWidth, w);
  set_number(kIOSurfaceHeight, h);
  set_number(kIOSurfaceBytesPerElement, bytes_per_element);
  set_number(kIOSurfaceBytesPerRow, bytes_per_row);
  set_number(kIOSurfacePixelFormat, pixel_format);

  IOSurfaceRef surface = IOSurfaceCreate(props);
  CFRelease(props);
  if (surface == nullptr) {
    ReleaseResources();
    return;
  }

  surface_ = surface;
  surface_id_ = IOSurfaceGetID(surface);

  OpenFrameCounter();

  // 先用一张黑帧把 surface 填上：消费端第一次导入时就有内容可画，
  // 而不是"还没来帧所以是一块未初始化的显存"。
  if (IOSurfaceLock(surface, 0, nullptr) == kIOReturnSuccess) {
    auto* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface));
    if (base != nullptr) {
      std::memset(base, 0,
                  static_cast<size_t>(IOSurfaceGetBytesPerRow(surface)) * height_);
    }
    IOSurfaceUnlock(surface, 0, nullptr);
  }

  valid_ = true;
}

IOSurfaceVideoSink::~IOSurfaceVideoSink() {
  ReleaseResources();
  delete static_cast<std::mutex*>(frame_mutex_);
  delete static_cast<std::vector<uint8_t>*>(scale_y_);
  delete static_cast<std::vector<uint8_t>*>(scale_u_);
  delete static_cast<std::vector<uint8_t>*>(scale_v_);
  delete static_cast<std::vector<uint8_t>*>(bgra_);
}

void IOSurfaceVideoSink::ReleaseResources() {
  if (counter_view_ != nullptr) {
    munmap(counter_view_, static_cast<size_t>(kCounterSize));
    counter_view_ = nullptr;
  }
  if (counter_fd_ >= 0) {
    // 计数器文件用完就删：/tmp 里别攒一堆 16 字节的小文件。
    // 已经映射过的消费端不受影响（POSIX：unlink 后既有映射照旧有效）。
    char name[64]{};
    std::snprintf(name, sizeof(name), "/tmp/livekit_gpu_frame_%016llX",
                  static_cast<unsigned long long>(surface_handle()));
    close(counter_fd_);
    unlink(name);
    counter_fd_ = -1;
  }
  if (surface_ != nullptr) {
    CFRelease(static_cast<IOSurfaceRef>(surface_));
    surface_ = nullptr;
  }
  surface_id_ = 0;
  valid_ = false;
}

void IOSurfaceVideoSink::OpenFrameCounter() {
  // 与 Windows 版的命名共享内存等价：`/tmp/livekit_gpu_frame_<句柄>`。
  // C# 侧用同一个路径 `MemoryMappedFile.CreateFromFile` 打开（Unix 上 .NET 的命名映射
  // 就是文件映射，这里直接明着用文件，省得去猜 .NET 的内部命名规则）。
  char name[64]{};
  std::snprintf(name, sizeof(name), "/tmp/livekit_gpu_frame_%016llX",
                static_cast<unsigned long long>(surface_handle()));
  counter_fd_ = open(name, O_CREAT | O_RDWR, 0600);
  if (counter_fd_ < 0) {
    return;
  }
  if (ftruncate(counter_fd_, kCounterSize) != 0) {
    close(counter_fd_);
    counter_fd_ = -1;
    return;
  }
  void* view = mmap(nullptr, kCounterSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                    counter_fd_, 0);
  if (view == MAP_FAILED) {
    close(counter_fd_);
    counter_fd_ = -1;
    return;
  }
  counter_view_ = view;
  counter_size_ = kCounterSize;
  auto* slots = static_cast<volatile int32_t*>(counter_view_);
  slots[kFrameIdSlot] = 0;
  slots[kProducerActiveSlot] = 1;
  slots[kRateLimitedDropSlot] = 0;
  slots[kConsumerAbsentDropSlot] = 0;
  slots[kSrcWidthSlot] = 0;
  slots[kSrcHeightSlot] = 0;
}

void IOSurfaceVideoSink::set_producer_active(bool active) const {
  if (counter_view_ != nullptr) {
    static_cast<volatile int32_t*>(counter_view_)[kProducerActiveSlot] =
        active ? 1 : 0;
  }
}

void IOSurfaceVideoSink::retire_consumer_handshake() {
  // mac 上没有 keyed mutex 要"还给消费端"：把 producer_active 归零就够 ——
  // 消费端发现没人再写就不会再发起 Update。
  set_producer_active(false);
}

void IOSurfaceVideoSink::CountDrop(int slot) const {
  if (counter_view_ != nullptr) {
    auto* slots = static_cast<volatile int32_t*>(counter_view_);
    slots[slot] = slots[slot] + 1;
  }
}

void IOSurfaceVideoSink::OnFrame(const webrtc::VideoFrame& frame) {
  if (frame_mutex_ == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(*static_cast<std::mutex*>(frame_mutex_));
  if (!valid_ || surface_ == nullptr || counter_view_ == nullptr) {
    return;
  }
  if (static_cast<volatile int32_t*>(counter_view_)[kProducerActiveSlot] == 0) {
    CountDrop(kConsumerAbsentDropSlot);
    return;
  }
  if (now_ms() < skip_until_ms_) {
    CountDrop(kConsumerAbsentDropSlot);
    return;
  }

  // 本端（观看侧）写 surface 的限速：还没到下一次允许写的时间 → 整帧丢掉。
  // **必须在转换之前**：mac 上虽然不用上传，但每帧的 I420→BGRA 转换本身也要 CPU。
  if (min_interval_ms_ > 0 && now_ms() - last_write_ms_ < min_interval_ms_) {
    CountDrop(kRateLimitedDropSlot);
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

  auto* surface = static_cast<IOSurfaceRef>(surface_);
  if (IOSurfaceLock(surface, 0, nullptr) != kIOReturnSuccess) {
    skip_until_ms_ = now_ms() + kConsumerAbsentBackoffMs;
    return;
  }

  auto* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const size_t surface_stride = IOSurfaceGetBytesPerRow(surface);
  if (base == nullptr || surface_stride < static_cast<size_t>(width_) * 4) {
    IOSurfaceUnlock(surface, 0, nullptr);
    return;
  }

  const double scale = std::min(
      static_cast<double>(width_) / src_width,
      static_cast<double>(height_) / src_height);
  const int dst_width = std::max(2, static_cast<int>(src_width * scale) & ~1);
  const int dst_height = std::max(2, static_cast<int>(src_height * scale) & ~1);
  const int dst_x = (static_cast<int>(width_) - dst_width) / 2;
  const int dst_y = (static_cast<int>(height_) - dst_height) / 2;

  const bool needs_scale = dst_width != src_width || dst_height != src_height;
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
    scaled_u.resize(static_cast<size_t>(dst_chroma_stride) *
                    ((dst_height + 1) / 2));
    scaled_v.resize(static_cast<size_t>(dst_chroma_stride) *
                    ((dst_height + 1) / 2));

    const double downscale_ratio =
        static_cast<double>(src_width) / static_cast<double>(dst_width);
    const libyuv::FilterMode scale_filter =
        downscale_ratio >= 2.0 ? libyuv::kFilterBox : libyuv::kFilterLinear;

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

  if (needs_clear) {
    // 只有"内容没铺满 surface"（留黑边）时才需要整面清零。
    std::memset(base, 0, surface_stride * height_);
  }

  // 与 Windows 版的一处**关键差异**：这里**不做上下翻转**。
  // Metal 的 2D 纹理原点在左上，libyuv 也是自上而下写，两边一致；
  // 消费端会按 `TopLeftOrigin = true` 告诉我们这一约定（见 GpuTexturePresenter）。
  // 万一在 mac 上看到画面倒过来：把这里的 +plane_height 改成 -plane_height、
  // 并把 dst_first_row 挪到最后一行（Windows 版就是这么翻的）。
  uint8_t* dst_first_row =
      base + static_cast<size_t>(dst_y) * surface_stride +
      static_cast<size_t>(dst_x) * 4;
  libyuv::I420ToARGB(
      plane_y, plane_y_stride, plane_u, plane_chroma_stride, plane_v,
      plane_chroma_stride, dst_first_row, static_cast<int>(surface_stride),
      plane_width, plane_height);

  IOSurfaceUnlock(surface, 0, nullptr);

  last_write_ms_ = now_ms();
  auto* slots = static_cast<volatile int32_t*>(counter_view_);
  // 先发布"这一帧的源尺寸"，再 +1 帧号：观看端看到新帧号时尺寸一定已经就绪。
  slots[kSrcWidthSlot] = src_width;
  slots[kSrcHeightSlot] = src_height;
  slots[kFrameIdSlot] = slots[kFrameIdSlot] + 1;
}

void IOSurfaceVideoSink::OnDiscardedFrame() {}

// ---------------- 冒烟测试支持（正式路径不调用）----------------

void test_feed_split_frame_iosurface(
    const std::shared_ptr<IOSurfaceVideoSink>& sink, uint32_t width,
    uint32_t height) {
  if (!sink || width < 4 || height < 4) {
    return;
  }

  const int w = static_cast<int>(width);
  const int h = static_cast<int>(height);
  const int half = h / 2;
  // BT.601 纯红：Y=76 / U=84 / V=255；纯蓝：Y=29 / U=255 / V=107。
  webrtc::scoped_refptr<webrtc::I420Buffer> buffer =
      webrtc::I420Buffer::Create(w, h);
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

int32_t test_frame_counter_iosurface(
    const std::shared_ptr<IOSurfaceVideoSink>& sink) {
  if (!sink || sink->counter_view_ == nullptr) {
    return -1;
  }
  return static_cast<volatile int32_t*>(sink->counter_view_)[kFrameIdSlot];
}

bool test_read_drop_counters_iosurface(
    const std::shared_ptr<IOSurfaceVideoSink>& sink, int32_t& rate_limited,
    int32_t& consumer_absent) {
  if (!sink || sink->counter_view_ == nullptr) {
    return false;
  }
  auto* slots = static_cast<volatile int32_t*>(sink->counter_view_);
  rate_limited = slots[kRateLimitedDropSlot];
  consumer_absent = slots[kConsumerAbsentDropSlot];
  return true;
}

bool test_read_frame_size_iosurface(
    const std::shared_ptr<IOSurfaceVideoSink>& sink, uint32_t& width,
    uint32_t& height) {
  if (!sink || sink->counter_view_ == nullptr) {
    return false;
  }
  auto* slots = static_cast<volatile int32_t*>(sink->counter_view_);
  width = static_cast<uint32_t>(slots[kSrcWidthSlot]);
  height = static_cast<uint32_t>(slots[kSrcHeightSlot]);
  return width > 0 && height > 0;
}

bool test_read_first_pixel(const std::shared_ptr<IOSurfaceVideoSink>& sink,
                          uint32_t& first_pixel) {
  if (!sink || sink->surface_ == nullptr) {
    return false;
  }
  auto* surface = static_cast<IOSurfaceRef>(sink->surface_);
  if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) !=
      kIOReturnSuccess) {
    return false;
  }
  bool ok = false;
  const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
  if (base != nullptr) {
    first_pixel = *reinterpret_cast<const uint32_t*>(base);
    ok = true;
  }
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
  return ok;
}

uint64_t IOSurfaceVideoSink::surface_handle() const {
  return reinterpret_cast<uint64_t>(surface_);
}

uint32_t IOSurfaceVideoSink::surface_id() const { return surface_id_; }
uint32_t IOSurfaceVideoSink::width() const { return width_; }
uint32_t IOSurfaceVideoSink::height() const { return height_; }
uint32_t IOSurfaceVideoSink::max_fps() const { return max_fps_; }
bool IOSurfaceVideoSink::valid() const { return valid_; }

void retire_iosurface_video_sink(
    const std::shared_ptr<IOSurfaceVideoSink>& sink) {
  if (!sink) {
    return;
  }

  sink->retire_consumer_handshake();
  // 与 Windows 版同样的"保活一小会儿"：视觉/消费端可能还持有这个 ref 做最后一次合成，
  // 立刻销毁 surface 会让那次合成读到空对象。这里只是把 shared_ptr 多留 10 秒。
  std::thread([sink]() {
    std::this_thread::sleep_for(std::chrono::seconds(10));
  }).detach();
}

std::shared_ptr<IOSurfaceVideoSink> new_iosurface_video_sink(uint32_t width,
                                                            uint32_t height,
                                                            uint32_t max_fps) {
  return std::make_shared<IOSurfaceVideoSink>(width, height, max_fps);
}

}  // namespace livekit_ffi

#endif  // defined(__APPLE__)
