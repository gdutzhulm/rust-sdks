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
        include!("livekit/d3d11_video_sink.h");
        include!("livekit/video_track.h");

        type VideoTrack = crate::video_track::ffi::VideoTrack;
        type D3D11VideoSink;

        fn new_d3d11_video_sink(width: u32, height: u32) -> SharedPtr<D3D11VideoSink>;
        fn add_d3d11_sink(
            self: &VideoTrack,
            sink: &SharedPtr<D3D11VideoSink>,
        );
        fn remove_d3d11_sink(
            self: &VideoTrack,
            sink: &SharedPtr<D3D11VideoSink>,
        );
        fn shared_handle(self: &D3D11VideoSink) -> u64;
        fn width(self: &D3D11VideoSink) -> u32;
        fn height(self: &D3D11VideoSink) -> u32;
        fn valid(self: &D3D11VideoSink) -> bool;
        fn set_producer_active(self: &D3D11VideoSink, active: bool);
        fn retire_d3d11_video_sink(sink: &SharedPtr<D3D11VideoSink>);

        // 冒烟测试支持（正式路径不使用）
        fn test_feed_split_frame(
            sink: &SharedPtr<D3D11VideoSink>,
            width: u32,
            height: u32,
        );
        fn test_consume_once(sink: &SharedPtr<D3D11VideoSink>) -> bool;
        fn test_read_texture(
            sink: &SharedPtr<D3D11VideoSink>,
            top_left_pixel: &mut u32,
            bottom_left_pixel: &mut u32,
        ) -> bool;
        fn test_frame_counter(sink: &SharedPtr<D3D11VideoSink>) -> i32;
    }
}

impl_thread_safety!(ffi::D3D11VideoSink, Send + Sync);
#[cfg(test)]
mod tests {
    use super::ffi;

    #[test]
    fn creates_d3d11_shared_texture() {
        let sink = ffi::new_d3d11_video_sink(1920, 1080);
        assert!(sink.valid());
        assert_ne!(sink.shared_handle(), 0);
    }

    /// 端到端跑一遍"解码帧 → 共享纹理 → 消费端读回"（不需要对端、不需要房间）。
    ///
    /// 重点锁住**上下方向**：Avalonia 走 ANGLE 采样（原点在左下），所以纹理里必须"倒"着存 ——
    /// 源图第 0 行要落在纹理最后一行。翻转以前是"写完再整帧行交换"（11MB 遍历），现在改成
    /// libyuv 的负 height 让它在转换里顺手完成；这个用例就是防止那次改写把画面弄倒。
    #[test]
    fn shared_texture_flip_and_frame_counter() {
        let sink = ffi::new_d3d11_video_sink(64, 32);
        assert!(sink.valid());

        // 构造函数写过一帧黑色并把钥匙交给了消费端；消费端先"显示"一次。
        assert!(ffi::test_consume_once(&sink));

        ffi::test_feed_split_frame(&sink, 64, 32);
        assert_eq!(ffi::test_frame_counter(&sink), 1, "消费端在场时应写成一帧");

        let (mut top, mut bottom) = (0u32, 0u32);
        assert!(ffi::test_read_texture(&sink, &mut top, &mut bottom));

        // BGRA（低字节为 B）：源图上半是红 → 落在纹理最后一行；源图下半是蓝 → 落在纹理第 0 行。
        let (top_b, _, top_r) = unpack(top);
        let (bottom_b, _, bottom_r) = unpack(bottom);
        assert!(
            top_b > 200 && top_r < 60,
            "纹理第 0 行应来自源图下半（蓝），实际 BGRA={:#010x}",
            top
        );
        assert!(
            bottom_r > 200 && bottom_b < 60,
            "纹理最后一行应来自源图上半（红），实际 BGRA={:#010x}",
            bottom
        );

        // 连续出帧：生产端不会被上一次显示卡死（帧序号继续涨）。
        ffi::test_feed_split_frame(&sink, 64, 32);
        assert_eq!(
            ffi::test_frame_counter(&sink),
            2,
            "生产端应能持续写帧"
        );
    }

    fn unpack(pixel: u32) -> (u8, u8, u8) {
        (
            (pixel & 0xFF) as u8,
            ((pixel >> 8) & 0xFF) as u8,
            ((pixel >> 16) & 0xFF) as u8,
        )
    }
}

