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
  D3D11VideoSink(uint32_t width, uint32_t height);
  ~D3D11VideoSink() override;

  void OnFrame(const webrtc::VideoFrame& frame) override;
  void OnDiscardedFrame() override;

  uint64_t shared_handle() const;
  uint32_t width() const;
  uint32_t height() const;
  bool valid() const;

  void set_producer_active(bool active) const;
  void retire_consumer_handshake();

 private:
  void ReleaseResources();

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool valid_ = false;
  void* device_ = nullptr;
  void* context_ = nullptr;
  void* texture_ = nullptr;
  void* keyed_mutex_ = nullptr;
  uint64_t shared_handle_ = 0;

  // 命名共享内存中的帧序号：C# 只在序号变化时刷新 Composition surface，
  // 避免原生暂停出帧时 C# 一直等待 keyed mutex。
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
  friend void test_release_consumer_key(const std::shared_ptr<D3D11VideoSink>& sink);
  friend bool test_read_texture(const std::shared_ptr<D3D11VideoSink>& sink,
                                uint32_t& top_left_pixel,
                                uint32_t& bottom_left_pixel);
  friend int32_t test_frame_counter(const std::shared_ptr<D3D11VideoSink>& sink);
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
void test_release_consumer_key(const std::shared_ptr<D3D11VideoSink>& sink);

/// 读回共享纹理的左上/左下角像素（BGRA 打包进 uint32，低字节为 B）。
bool test_read_texture(const std::shared_ptr<D3D11VideoSink>& sink,
                       uint32_t& top_left_pixel,
                       uint32_t& bottom_left_pixel);

/// 读共享内存里的帧序号（原生每写成一帧 +1）。
int32_t test_frame_counter(const std::shared_ptr<D3D11VideoSink>& sink);

void retire_d3d11_video_sink(const std::shared_ptr<D3D11VideoSink>& sink);

std::shared_ptr<D3D11VideoSink> new_d3d11_video_sink(uint32_t width,
                                                    uint32_t height);

}  // namespace livekit_ffi
