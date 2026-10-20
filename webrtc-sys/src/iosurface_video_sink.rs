// Copyright 2025 LiveKit, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use crate::impl_thread_safety;

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    unsafe extern "C++" {
        include!("livekit/iosurface_video_sink.h");
        include!("livekit/video_track.h");

        type VideoTrack = crate::video_track::ffi::VideoTrack;
        type IOSurfaceVideoSink;

        fn new_iosurface_video_sink(
            width: u32,
            height: u32,
            max_fps: u32,
        ) -> SharedPtr<IOSurfaceVideoSink>;
        fn add_iosurface_sink(
            self: &VideoTrack,
            sink: &SharedPtr<IOSurfaceVideoSink>,
        );
        fn remove_iosurface_sink(
            self: &VideoTrack,
            sink: &SharedPtr<IOSurfaceVideoSink>,
        );
        /// 传给 C# 的"共享句柄"：IOSurfaceRef 指针（同进程内有效，Avalonia 就是这么要的）。
        fn surface_handle(self: &IOSurfaceVideoSink) -> u64;
        /// IOSurface 全局 ID（日志/诊断用）。
        fn surface_id(self: &IOSurfaceVideoSink) -> u32;
        fn width(self: &IOSurfaceVideoSink) -> u32;
        fn height(self: &IOSurfaceVideoSink) -> u32;
        fn max_fps(self: &IOSurfaceVideoSink) -> u32;
        fn valid(self: &IOSurfaceVideoSink) -> bool;
        fn set_producer_active(self: &IOSurfaceVideoSink, active: bool);
        fn retire_iosurface_video_sink(sink: &SharedPtr<IOSurfaceVideoSink>);

        // 冒烟测试支持（正式路径不使用）
        fn test_feed_split_frame_iosurface(
            sink: &SharedPtr<IOSurfaceVideoSink>,
            width: u32,
            height: u32,
        );
        fn test_frame_counter_iosurface(sink: &SharedPtr<IOSurfaceVideoSink>) -> i32;
        fn test_read_drop_counters_iosurface(
            sink: &SharedPtr<IOSurfaceVideoSink>,
            rate_limited: &mut i32,
            consumer_absent: &mut i32,
        ) -> bool;
        fn test_read_frame_size_iosurface(
            sink: &SharedPtr<IOSurfaceVideoSink>,
            width: &mut u32,
            height: &mut u32,
        ) -> bool;
        fn test_read_first_pixel(
            sink: &SharedPtr<IOSurfaceVideoSink>,
            first_pixel: &mut u32,
        ) -> bool;
    }
}

impl_thread_safety!(ffi::IOSurfaceVideoSink, Send + Sync);

#[cfg(test)]
mod tests {
    use super::ffi;

    #[test]
    fn creates_iosurface() {
        let sink = ffi::new_iosurface_video_sink(1920, 1080, 0);
        assert!(sink.valid(), "IOSurface 应该建成功");
        assert_ne!(sink.surface_handle(), 0, "应该导出 IOSurfaceRef 句柄");
        assert_ne!(sink.surface_id(), 0, "IOSurface 应该有全局 ID");
    }

    /// 端到端跑一遍"解码帧 → IOSurface → 消费端读回"（不需要对端、不需要房间）。
    ///
    /// 重点锁住**上下方向**：mac 上 Metal 与 libyuv 都是自上而下，所以这里**不翻转** ——
    /// 源图第 0 行落在 surface 第 0 行。C# 侧会按 `TopLeftOrigin = true` 告诉 Avalonia 这一约定。
    #[test]
    fn writes_frame_top_down_and_counts() {
        let sink = ffi::new_iosurface_video_sink(64, 32, 0);
        assert!(sink.valid());

        ffi::test_feed_split_frame_iosurface(&sink, 64, 32);
        assert_eq!(ffi::test_frame_counter_iosurface(&sink), 1, "应该写成一帧");

        // 与 Windows 版同一契约：slot 5/6 = 解码后的真实帧尺寸（C# 靠它发现竖横屏切换）。
        let (mut src_w, mut src_h) = (0u32, 0u32);
        assert!(ffi::test_read_frame_size_iosurface(&sink, &mut src_w, &mut src_h));
        assert_eq!((src_w, src_h), (64, 32), "slot 5/6 应该是解码后的真实帧尺寸");

        let mut first_pixel = 0u32;
        assert!(ffi::test_read_first_pixel(&sink, &mut first_pixel));
        let (blue, _, red) = unpack(first_pixel);
        assert!(
            red > 200 && blue < 60,
            "surface 第 0 行应来自**源图上半（红）**（mac 不翻转），实际 BGRA={:#010x}",
            first_pixel
        );
    }

    /// 生产端限速：限速窗口内的帧在**转换之前**就被丢掉（连 I420→BGRA 都不做）。
    #[test]
    fn max_fps_drops_frames_before_conversion() {
        let sink = ffi::new_iosurface_video_sink(64, 32, 1); // 1fps → 两帧至少隔 1s
        assert!(sink.valid());
        assert_eq!(sink.max_fps(), 1);

        ffi::test_feed_split_frame_iosurface(&sink, 64, 32);
        assert_eq!(ffi::test_frame_counter_iosurface(&sink), 1);

        ffi::test_feed_split_frame_iosurface(&sink, 64, 32);
        assert_eq!(
            ffi::test_frame_counter_iosurface(&sink),
            1,
            "1s 内的第二帧应该被限速丢掉（帧号不涨）"
        );

        let (mut rate_limited, mut consumer_absent) = (-1i32, -1i32);
        assert!(ffi::test_read_drop_counters_iosurface(
            &sink,
            &mut rate_limited,
            &mut consumer_absent
        ));
        assert_eq!(rate_limited, 1, "被限速丢掉的那一帧应该计数");
        assert_eq!(consumer_absent, 0);
    }

    fn unpack(pixel: u32) -> (u8, u8, u8) {
        (
            (pixel & 0xFF) as u8,
            ((pixel >> 8) & 0xFF) as u8,
            ((pixel >> 16) & 0xFF) as u8,
        )
    }
}
