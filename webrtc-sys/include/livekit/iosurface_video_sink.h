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

/// macOS 侧的"视频直出"：把解码/采集帧按 BGRA 写进一块 **IOSurface**，
/// 由 Avalonia 的 Metal 合成器按 `KnownPlatformGraphicsExternalImageHandleTypes.IOSurfaceRef`
/// 导入（`MTLTexture` → 合成）。它是 Windows 上 `D3D11VideoSink` 的对等实现。
///
/// 与 Windows 版的差别（为什么 mac 上更简单）：
///   · IOSurface 的内存**本来就可被 CPU 直接写**，不需要 D3D11 那套 `UpdateSubresource`
///     上传 + keyed mutex 轮转；所以这里只做 "lock → libyuv 转 BGRA → unlock"。
///   · 没有 keyed mutex：消费端（Avalonia）要么走 `UpdateAsync`，要么按后端声明的
///     同步能力选 keyed-mutex；我们的限速/丢帧计数照旧，用来把"白做的帧"省掉。
///   · 句柄给的是 **IOSurfaceRef 指针**（同进程内有效，正是 Avalonia 期望的形式：
///     它在 ObjC 侧直接拿着这个 ref 建纹理，不做 IOSurfaceLookup）。
class IOSurfaceVideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  IOSurfaceVideoSink(uint32_t width, uint32_t height, uint32_t max_fps);
  ~IOSurfaceVideoSink() override;

  void OnFrame(const webrtc::VideoFrame& frame) override;
  void OnDiscardedFrame() override;

  /// 传给 C# 的"共享句柄"：IOSurfaceRef 指针（同进程内有效）。
  uint64_t surface_handle() const;
  /// IOSurface 的全局 ID（诊断/日志用）。
  uint32_t surface_id() const;
  uint32_t width() const;
  uint32_t height() const;
  uint32_t max_fps() const;
  bool valid() const;

  void set_producer_active(bool active) const;
  /// 消费端可能正等着"下一帧"：这里不阻塞，只是把 producer_active 归零（mac 上没有锁要还）。
  void retire_consumer_handshake();

 private:
  void ReleaseResources();

  /// 记一次"丢掉"（写计数器，C# 用它算"到 sink 的帧数 vs 实际写入帧数"）。
  void CountDrop(int slot) const;
  /// 建/映射计数器共享内存（`/tmp/livekit_gpu_frame_<surface_id>`，C# 用同一个路径打开）。
  void OpenFrameCounter();

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t max_fps_ = 0;
  bool valid_ = false;

  void* surface_ = nullptr;   // IOSurfaceRef
  uint32_t surface_id_ = 0;

  // 生产端限速（0 = 不限），语义与 Windows 版完全一致。
  double min_interval_ms_ = 0;
  double last_write_ms_ = 0;

  // 消费端不在场时的退避截止时间（steady_clock 毫秒）。
  double skip_until_ms_ = 0;

  // 帧号 / producer_active / 限速丢弃 / 无宿主丢弃（与 Windows 版同一套槽位布局）。
  int counter_fd_ = -1;
  void* counter_view_ = nullptr;
  int counter_size_ = 0;

  // Reused conversion buffers. All access is serialized by frame_mutex_.
  void* frame_mutex_ = nullptr;
  void* scale_y_ = nullptr;
  void* scale_u_ = nullptr;
  void* scale_v_ = nullptr;
  void* bgra_ = nullptr;

  // 冒烟测试支持（用例见 webrtc-sys/src/iosurface_video_sink.rs）
  friend void test_feed_split_frame_iosurface(const std::shared_ptr<IOSurfaceVideoSink>& sink,
                                              uint32_t width,
                                              uint32_t height);
  friend int32_t test_frame_counter_iosurface(const std::shared_ptr<IOSurfaceVideoSink>& sink);
  friend bool test_read_drop_counters_iosurface(const std::shared_ptr<IOSurfaceVideoSink>& sink,
                                                int32_t& rate_limited,
                                                int32_t& consumer_absent);
  friend bool test_read_first_pixel(const std::shared_ptr<IOSurfaceVideoSink>& sink,
                                    uint32_t& first_pixel);
};

/// 造一张"上半纯红 / 下半纯蓝"的 I420 帧，走完整的 OnFrame 路径写进 IOSurface。
void test_feed_split_frame_iosurface(const std::shared_ptr<IOSurfaceVideoSink>& sink,
                                     uint32_t width,
                                     uint32_t height);

/// 读计数器里的帧号（每写成一帧 +1）。
int32_t test_frame_counter_iosurface(const std::shared_ptr<IOSurfaceVideoSink>& sink);

/// 读丢弃计数：被限速挡在转换之前的 / 消费端不在场丢掉的。
bool test_read_drop_counters_iosurface(const std::shared_ptr<IOSurfaceVideoSink>& sink,
                                       int32_t& rate_limited,
                                       int32_t& consumer_absent);

/// 读 IOSurface 第 0 行第 0 个像素（BGRA 打包进 uint32，低字节为 B）。
bool test_read_first_pixel(const std::shared_ptr<IOSurfaceVideoSink>& sink,
                           uint32_t& first_pixel);

void retire_iosurface_video_sink(const std::shared_ptr<IOSurfaceVideoSink>& sink);

std::shared_ptr<IOSurfaceVideoSink> new_iosurface_video_sink(uint32_t width,
                                                             uint32_t height,
                                                             uint32_t max_fps);

}  // namespace livekit_ffi
