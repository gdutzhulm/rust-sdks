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

#pragma once

#include <cstdint>
#include <memory>

#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"

namespace livekit_ffi {

/// Receives decoded WebRTC video frames and publishes them into a
/// D3D11 shared texture that can be imported by Avalonia's composition GPU
/// interop API. Only implemented on Windows.
class D3D11VideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  D3D11VideoSink(uint32_t width, uint32_t height, uint32_t max_fps);
  ~D3D11VideoSink() override;

  void OnFrame(const webrtc::VideoFrame& frame) override;
  void OnDiscardedFrame() override;

  uint64_t shared_handle() const;
  uint32_t width() const;
  uint32_t height() const;
  uint32_t max_fps() const;
  bool valid() const;

  void set_producer_active(bool active) const;
  void retire_consumer_handshake();

 private:
  void ReleaseResources();

  /// 把 keyed mutex 的钥匙**交还消费端**（唤醒可能正在等 key 1 的合成线程）。
  ///
  /// ★ **只允许在生产端的终态调用**：`retire_consumer_handshake()`、`ReleaseResources()`，以及
  /// "根本没有共享内存可写"那条路（`OnFrame` 里 `frame_view_ == nullptr`）。
  ///
  /// 为什么不能在中途调：keyed mutex 是**严格轮转**（生产端 acquire(0) → release(1)；消费端
  /// acquire(1) → release(0)，空闲时只能按"上一次 release 的 key"取）。生产端要写帧就必须让状态停在
  /// "key 0 可用"；而观看侧两次显示之间会主动"按住"我们（`producer_active=0`）以省掉一次转换+上传，
  /// 下一拍再松手让我们写。若在被按住/限速/退避这些"这一拍不写"的分支里交还钥匙，状态就变成
  /// "key 1 可用"，松手后 `AcquireSync(0)` 会一直失败，而消费端只在帧号变化时才合成 ⇒ **帧号永远不动**
  /// （2026-10-13 VDI 现场：帧号停在 2、4.5 秒后被判"没有新帧"而回退位图）。
  ///
  /// 非阻塞探测：只有"无人持有 + 上次 release 的是 key 0"才拿得到，拿到就 release(1)（内容不动）；
  /// 其余情况空操作。调用方必须已持有 frame_mutex_。
  /// <param name="timeout_ms">0 = 非阻塞；销毁路径传几十毫秒，先让消费端拷完再交还。</param>
  void HandBackConsumerKeyLocked(uint32_t timeout_ms) const;

  /// 记一次"丢掉"（写共享内存里的计数，C# 用它算"到 sink 的帧数 vs 实际写入帧数"）。
  /// slot 布局见 cpp 里的 kFrameSlotCount 说明。
  void CountDrop(int slot) const;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool valid_ = false;

  // 生产端限速（0 = 不限）：两次"写成一帧"之间至少隔 min_interval_ms_。
  // 判断放在**转换之前** —— 被丢的帧连 I420->BGRA 与上传都不做，所以
  // VDI（无独显、Avalonia 走软渲染）上这条路的成本与**显示帧率**成正比，
  // 而不是与"远端推流帧率"（实测 30~40fps）成正比。
  uint32_t max_fps_ = 0;
  double min_interval_ms_ = 0;
  double last_write_ms_ = 0;
  void* device_ = nullptr;
  void* context_ = nullptr;
  void* texture_ = nullptr;
  void* keyed_mutex_ = nullptr;
  uint64_t shared_handle_ = 0;

  // 命名共享内存：4 个 LONG（帧序号 / producer_active / 限速丢弃 / 消费端不在场丢弃）。
  // C# 只在帧序号变化时才去刷新 Composition surface，避免原生暂停出帧时 C# 一直等 keyed mutex；
  // 另外两个计数给 C# 算"到 sink 的帧数 ≈ 写入 + 限速丢 + 无宿主丢"（VDI 上最关心的那笔账）。
  void* frame_mapping_ = nullptr;
  void* frame_view_ = nullptr;

  // Reused conversion buffers. All access is serialized by frame_mutex_.
  void* frame_mutex_ = nullptr;
  void* scale_y_ = nullptr;
  void* scale_u_ = nullptr;
  void* scale_v_ = nullptr;
  void* bgra_ = nullptr;

  // 消费端不在场时的退避截止时间（steady_clock 毫秒）。keyed mutex 是"生产者 acquire(0) →
  // 消费者 acquire(1)"轮转的，消费端消失时生产端会一直拿不到 key；老实现用 INFINITE 等，
  // 会把 WebRTC 的视频线程永久卡住。现在改成"拿不到就丢这一帧 + 退避一段时间再试"。
  double skip_until_ms_ = 0;

  // 冒烟测试支持（正式路径不使用；用例见 webrtc-sys/src/d3d11_video_sink.rs）：
  // 直接操作内部句柄，因此走 friend 而不是公开访问器。
  friend void test_feed_split_frame(const std::shared_ptr<D3D11VideoSink>& sink,
                                    uint32_t width, uint32_t height);
  friend bool test_consume_once(const std::shared_ptr<D3D11VideoSink>& sink);
  friend bool test_acquire_consumer_key(const std::shared_ptr<D3D11VideoSink>& sink);
  friend bool test_acquire_consumer_key_for(
      const std::shared_ptr<D3D11VideoSink>& sink, uint32_t timeout_ms);
  friend void test_release_consumer_key(const std::shared_ptr<D3D11VideoSink>& sink);
  friend bool test_read_texture(const std::shared_ptr<D3D11VideoSink>& sink,
                                uint32_t& top_left_pixel,
                                uint32_t& bottom_left_pixel);
  friend int32_t test_frame_counter(const std::shared_ptr<D3D11VideoSink>& sink);
  friend int32_t test_handback_count(const std::shared_ptr<D3D11VideoSink>& sink);
  friend bool test_read_drop_counters(const std::shared_ptr<D3D11VideoSink>& sink,
                                      int32_t& rate_limited,
                                      int32_t& consumer_absent);
  friend bool test_read_frame_size(const std::shared_ptr<D3D11VideoSink>& sink,
                                 uint32_t& width, uint32_t& height);
};

// ---- 冒烟测试支持（正式路径不使用）----
// 目的：在没有对端、没有房间的情况下把这条链路的**协议**验证掉 ——
// keyed mutex 的轮转（生产端不会永久等待）、共享纹理的上下翻转方向、帧序号。

/// 造一张"上半纯红 / 下半纯蓝"的 I420 帧，走完整的 OnFrame 路径写进共享纹理。
void test_feed_split_frame(const std::shared_ptr<D3D11VideoSink>& sink,
                           uint32_t width, uint32_t height);

/// 模拟 Avalonia 消费一帧（acquire key 1 → release key 0）。钥匙不在消费端手上时返回 false。
bool test_consume_once(const std::shared_ptr<D3D11VideoSink>& sink);

/// 模拟"合成线程正在拷贝"（acquire key 1，先不还）与"拷完了"（release key 0）。
/// 用来验证生产端在消费端占着纹理时**不会无限等待**。
bool test_acquire_consumer_key(const std::shared_ptr<D3D11VideoSink>& sink);

/// 同 test_acquire_consumer_key，但可以给一个等待上限。
/// 消费端（Avalonia）真实用的是**无限等**，所以"拿不到 key 1"就等于"整个窗口不再重绘"。
bool test_acquire_consumer_key_for(const std::shared_ptr<D3D11VideoSink>& sink,
                                   uint32_t timeout_ms);

/// 读共享内存里的"生产端交还钥匙"累计次数（诊断：这条在涨 = 生产端一直在替消费端解套）。
int32_t test_handback_count(const std::shared_ptr<D3D11VideoSink>& sink);
void test_release_consumer_key(const std::shared_ptr<D3D11VideoSink>& sink);

/// 读回共享纹理的左上/左下角像素（BGRA 打包进 uint32，低字节为 B）。
bool test_read_texture(const std::shared_ptr<D3D11VideoSink>& sink,
                       uint32_t& top_left_pixel,
                       uint32_t& bottom_left_pixel);

/// 读共享内存里的帧序号（原生每写成一帧 +1）。
int32_t test_frame_counter(const std::shared_ptr<D3D11VideoSink>& sink);

/// 读共享内存里的丢弃计数：被限速挡在转换之前的 / 消费端不在场丢掉的。
bool test_read_drop_counters(const std::shared_ptr<D3D11VideoSink>& sink,
                             int32_t& rate_limited,
                             int32_t& consumer_absent);

/// 读共享内存里的“解码后真实帧宽高”（slot 5/6）。
bool test_read_frame_size(const std::shared_ptr<D3D11VideoSink>& sink,
                           uint32_t& width, uint32_t& height);

void retire_d3d11_video_sink(const std::shared_ptr<D3D11VideoSink>& sink);

std::shared_ptr<D3D11VideoSink> new_d3d11_video_sink(uint32_t width,
                                                    uint32_t height,
                                                    uint32_t max_fps);

}  // namespace livekit_ffi
