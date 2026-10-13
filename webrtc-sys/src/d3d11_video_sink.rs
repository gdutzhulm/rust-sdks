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

        fn new_d3d11_video_sink(
            width: u32,
            height: u32,
            max_fps: u32,
        ) -> SharedPtr<D3D11VideoSink>;
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
        fn max_fps(self: &D3D11VideoSink) -> u32;
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
        fn test_acquire_consumer_key(sink: &SharedPtr<D3D11VideoSink>) -> bool;
        fn test_release_consumer_key(sink: &SharedPtr<D3D11VideoSink>);
        fn test_read_texture(
            sink: &SharedPtr<D3D11VideoSink>,
            top_left_pixel: &mut u32,
            bottom_left_pixel: &mut u32,
        ) -> bool;
        fn test_frame_counter(sink: &SharedPtr<D3D11VideoSink>) -> i32;
        fn test_acquire_consumer_key_for(
            sink: &SharedPtr<D3D11VideoSink>,
            timeout_ms: u32,
        ) -> bool;
        fn test_handback_count(sink: &SharedPtr<D3D11VideoSink>) -> i32;
        fn test_read_drop_counters(
            sink: &SharedPtr<D3D11VideoSink>,
            rate_limited: &mut i32,
            consumer_absent: &mut i32,
        ) -> bool;
    }
}

impl_thread_safety!(ffi::D3D11VideoSink, Send + Sync);
#[cfg(test)]
mod tests {
    use super::ffi;

    #[test]
    fn creates_d3d11_shared_texture() {
        let sink = ffi::new_d3d11_video_sink(1920, 1080, 0);
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
        let sink = ffi::new_d3d11_video_sink(64, 32, 0);
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

    /// **回归用例（2026-10-13）**：生产端"这一拍不写像素"（限速 / 被按住 / 退避）时，
    /// **绝不能**把 keyed mutex 交还给消费端 —— 那会把生产端自己的轮次让出去。
    ///
    /// 为什么：keyed mutex 是**严格轮转**（空闲时只能按"上一次 release 的 key"取）。生产端要写帧，
    /// 状态就必须停在"key 0 可用"；而观看侧两次显示之间会主动按住我们（`producer_active=0`）来省掉
    /// 一次转换+上传，下一拍再松手让我们写。若在"不写"的分支里交还钥匙（状态变成 key 1 可用），
    /// 松手后 `AcquireSync(0)` 会一直失败，而消费端只在**帧号变化**时才去合成 ⇒ 帧号永远不动。
    /// VDI 现场就是这个指纹：帧号停在 2，4.5 秒后被判"没有新帧"而回退位图。
    #[test]
    fn skipped_frame_does_not_strand_the_producer() {
        let sink = ffi::new_d3d11_video_sink(64, 32, 1); // 1fps：1 秒内的第二帧必被限速丢掉
        assert!(sink.valid());

        assert!(ffi::test_consume_once(&sink));
        ffi::test_feed_split_frame(&sink, 64, 32);
        assert_eq!(ffi::test_frame_counter(&sink), 1, "第一帧应该写进去");
        assert!(ffi::test_consume_once(&sink), "消费端显示它 -> 钥匙轮到生产端");

        ffi::test_feed_split_frame(&sink, 64, 32); // 限速：这一拍不写像素
        assert_eq!(ffi::test_frame_counter(&sink), 1, "限速帧不该写进纹理");

        // 等限速窗口过去：生产端必须还能写（钥匙没被让出去）。
        std::thread::sleep(std::time::Duration::from_millis(1100));
        ffi::test_feed_split_frame(&sink, 64, 32);
        assert_eq!(
            ffi::test_frame_counter(&sink),
            2,
            "跳写之后生产端必须还能写下一帧（钥匙没被让给消费端）"
        );
    }

    /// 同上：被本端按住（观看侧省 CPU 的那一步）之后松手，生产端必须还能写。
    #[test]
    fn paused_frame_does_not_strand_the_producer() {
        let sink = ffi::new_d3d11_video_sink(64, 32, 0);
        assert!(sink.valid());

        assert!(ffi::test_consume_once(&sink));
        sink.set_producer_active(false); // 本端：这一拍不显示这块纹理
        ffi::test_feed_split_frame(&sink, 64, 32);
        assert_eq!(ffi::test_frame_counter(&sink), 0, "被按住时不该写纹理");

        sink.set_producer_active(true); // 本端松手：紧接着就等我们出帧
        ffi::test_feed_split_frame(&sink, 64, 32);
        assert_eq!(
            ffi::test_frame_counter(&sink),
            1,
            "松手后生产端必须还能写（钥匙没被让给消费端）"
        );
    }

    /// 流被释放（原生退役）时，必须把还在等 key 1 的消费端**叫醒** ——
    /// 否则那个合成线程会一直挂在纹理上，而调用方随后就会释放这一路原生流（"消费端还在用、原生在退役"）。
    #[test]
    fn retire_wakes_waiting_consumer() {
        let sink = ffi::new_d3d11_video_sink(64, 32, 0);
        assert!(sink.valid());

        assert!(ffi::test_consume_once(&sink));
        assert!(
            !ffi::test_acquire_consumer_key_for(&sink, 100),
            "这一拍钥匙在生产端手上，消费端本来就该等"
        );

        ffi::retire_d3d11_video_sink(&sink);
        assert!(
            ffi::test_acquire_consumer_key(&sink),
            "退役时必须把钥匙交给等着的消费端"
        );
        assert!(ffi::test_handback_count(&sink) >= 1, "终态交还钥匙应该被记数");
    }

    /// 生产端限速：max_fps 生效时，限速窗口内的帧**在转换之前**就被丢掉
    /// （连 I420->BGRA 都不做），所以帧序号不动 —— VDI 上 CPU 按显示帧率走。
    #[test]
    fn max_fps_drops_frames_before_conversion() {
        let sink = ffi::new_d3d11_video_sink(64, 32, 1); // 1fps -> 两帧至少隔 1s
        assert!(sink.valid());
        assert_eq!(sink.max_fps(), 1);

        assert!(ffi::test_consume_once(&sink));

        ffi::test_feed_split_frame(&sink, 64, 32);
        assert_eq!(
            ffi::test_frame_counter(&sink),
            1,
            "限速窗口内的第一帧应该写进去"
        );

        ffi::test_feed_split_frame(&sink, 64, 32);
        assert_eq!(
            ffi::test_frame_counter(&sink),
            1,
            "1s 内的第二帧应该被限速丢掉（帧序号不涨）"
        );

        // 更要紧的是"丢了多少"能报出来（C# 用它算 到 sink 的帧数 ≈ 写入 + 限速丢 + 无宿主丢）
        let (mut rate_limited, mut consumer_absent) = (-1i32, -1i32);
        assert!(ffi::test_read_drop_counters(
            &sink,
            &mut rate_limited,
            &mut consumer_absent
        ));
        assert_eq!(rate_limited, 1, "被限速丢掉的那一帧应该计数");
        assert_eq!(consumer_absent, 0, "消费端在场时不该记「无宿主丢弃」");
        // 注意：这一拍钥匙**已经在消费端那侧**（第一帧写完后没人取走），所以"交还"拿不到锁、计数不动。
        // "跳写时必须交还钥匙"由 producer_hands_back_key_when_it_skips_writing 覆盖。
    }

    fn unpack(pixel: u32) -> (u8, u8, u8) {
        (
            (pixel & 0xFF) as u8,
            ((pixel >> 8) & 0xFF) as u8,
            ((pixel >> 16) & 0xFF) as u8,
        )
    }
}

