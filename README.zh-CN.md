<!--BEGIN_BANNER_IMAGE-->

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="/.github/banner_dark.png">
  <source media="(prefers-color-scheme: light)" srcset="/.github/banner_light.png">
  <img style="width:100%;" alt="LiveKit 图标、仓库名，以及背景中的示例代码。" src="https://raw.githubusercontent.com/livekit/rust-sdks/main/.github/banner_light.png">
</picture>

<!--END_BANNER_IMAGE-->

# 📹🎙️🦀 LiveKit 的 Rust 客户端 SDK

[English](README.md) | [简体中文](README.zh-CN.md)

<!--BEGIN_DESCRIPTION-->
用这个 SDK 给你的 Rust 应用加上实时视频、音频和数据能力。连上 <a href="https://livekit.io/">LiveKit</a> 云服务或自建服务端，几行代码就能搭出多模态 AI、直播、视频通话这类应用。
<!--END_DESCRIPTION-->

[![crates.io](https://img.shields.io/crates/v/livekit.svg)](https://crates.io/crates/livekit)
[![livekit docs.rs](https://img.shields.io/docsrs/livekit)](https://docs.rs/livekit/latest/)
[![Builds](https://github.com/livekit/rust-sdks/actions/workflows/builds.yml/badge.svg?branch=main)](https://github.com/livekit/rust-sdks/actions/workflows/builds.yml)
[![Tests](https://github.com/livekit/rust-sdks/actions/workflows/tests.yml/badge.svg?branch=main)](https://github.com/livekit/rust-sdks/actions/workflows/tests.yml)

## 功能特性

- [x] 接收音视频轨道
- [x] 发布音视频轨道
- [x] 数据通道
- [x] Simulcast（同时推多档码率）
- [x] SVC 编解码（AV1/VP9）
- [ ] 自适应码流
- [x] Dynacast（动态转发）
- [x] 视频硬件编解码
  - [x] H.264、H.265，走 VideoToolbox（macOS/iOS）
  - [x] H.264、H.265，走 NVidia 独显（Linux）
  - [x] H.264、H.265，走 AMD CPU 与 GPU（Linux）
  - [x] H.264、H.265、AV1，走 NVidia Jetson（Linux）
- 支持的平台
  - [x] Windows
  - [x] macOS
  - [x] Linux
  - [x] iOS
  - [x] Android

## Crate 一览

- `livekit-api`：服务端 API 与访问令牌（access token）生成
- `livekit`：LiveKit 实时 SDK
- `livekit-ffi`：内部 crate，用来给其它语言生成绑定
- `livekit-protocol`：LiveKit 协议生成代码

把本 SDK 作为依赖加进你的项目时，记得给 cargo 配置加上[必需的 `rustflags`](https://github.com/livekit/rust-sdks/blob/main/.cargo/config.toml)，否则链接可能失败。

另外请参考[受支持的平台工具链清单](https://github.com/livekit/rust-sdks/blob/main/.github/workflows/builds.yml)。

## 快速开始

目前使用本 SDK 需要 Tokio，不过我们计划让异步执行器与运行时解耦。

## 使用服务端 API

### 生成访问令牌

```rust
use livekit_api::access_token;
use std::env;

fn create_token() -> Result<String, access_token::AccessTokenError> {
    let api_key = env::var("LIVEKIT_API_KEY").expect("LIVEKIT_API_KEY is not set");
    let api_secret = env::var("LIVEKIT_API_SECRET").expect("LIVEKIT_API_SECRET is not set");

    let token = access_token::AccessToken::with_api_key(&api_key, &api_secret)
        .with_identity("rust-bot")
        .with_name("Rust Bot")
        .with_grants(access_token::VideoGrants {
             room_join: true,
             room: "my-room".to_string(),
             ..Default::default()
        })
        .to_jwt();
    return token
}
```

### 用 RoomService API 创建房间

```rust
use livekit_api::services::room::{CreateRoomOptions, RoomClient};

#[tokio::main]
async fn main() {
    let room_service = RoomClient::new("http://localhost:7880").unwrap();

    let room = room_service
        .create_room("my_room", CreateRoomOptions::default())
        .await
        .unwrap();

    println!("Created room: {:?}", room);
}
```

## 使用实时 SDK

### 连接房间并监听事件：

```rust
use livekit::prelude::*;

#[tokio::main]
async fn main() -> Result<()> {
    let (room, mut room_events) = Room::connect(&url, &token).await?;

    while let Some(event) = room_events.recv().await {
        match event {
            RoomEvent::TrackSubscribed { track, publication, participant } => {
                // ...
            }
            _ => {}
        }
    }

    Ok(())
}
```

### 接收已订阅轨道的视频帧

```rust
...
use futures::StreamExt; // this trait is required for iterating on audio & video frames
use livekit::prelude::*;

match event {
    RoomEvent::TrackSubscribed { track, publication, participant } => {
        match track {
            RemoteTrack::Audio(audio_track) => {
                let rtc_track = audio_track.rtc_track();
                let mut audio_stream = NativeAudioStream::new(rtc_track);
                tokio::spawn(async move {
                    // Receive the audio frames in a new task
                    while let Some(audio_frame) = audio_stream.next().await {
                        log::info!("received audio frame - {audio_frame:#?}");
                    }
                });
            },
            RemoteTrack::Video(video_track) => {
                let rtc_track = video_track.rtc_track();
                let mut video_stream = NativeVideoStream::new(rtc_track);
                tokio::spawn(async move {
                    // Receive the video frames in a new task
                    while let Some(video_frame) = video_stream.next().await {
                        log::info!("received video frame - {video_frame:#?}");
                    }
                });
            },
        }
    },
    _ => {}
}
```

## 示例

![](https://github.com/livekit/rust-sdks/blob/main/examples/images/simple-room-demo.gif)

- [basic room](https://github.com/livekit/rust-sdks/tree/main/examples/basic_room)：连接房间的最简示例。
- [rust-dev-client](https://github.com/livekit-examples/rust-dev-client)：完整示例应用，用 wgpu + egui 做视频渲染（维护在另一个仓库里）。
- [mobile](https://github.com/livekit/rust-sdks/tree/main/examples/mobile)：面向 iOS 与 Android 的移动端示例
- [play_from_disk](https://github.com/livekit/rust-sdks/tree/main/examples/play_from_disk)：从 wav 文件发布音频
- [save_to_disk](https://github.com/livekit/rust-sdks/tree/main/examples/save_to_disk)：把收到的音频存成 wav 文件

## 构建

### macOS

在 macOS 上构建需要 `-ObjC` 链接参数 —— LiveKit 的 WebRTC 实现用到了 Mac 上的 Objective-C 库。如果应用没有链接 ObjC，可能会遇到下面这个错误：

```
*** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason: '-[RTCVideoCodecInfo nativeSdpVideoFormat]: unrecognized selector sent to instance 0x600003bc6660'
```

## 动机与设计目标

LiveKit 的目标是提供一套开源、端到端、到哪都能跑的 WebRTC 技术栈。就这个 SDK 而言，我们有两个目标：

1. 给 Rust 开发者一个独立的、跨平台的 LiveKit 客户端 SDK。
2. 给其它平台专用 SDK（比如 Unity、Unreal、iOS、Android）提供一个公共内核。

关于第 2 点：我们已经为多个平台做过[客户端 SDK](https://github.com/livekit?q=client-sdk&type=all)，过程中踩到几个坑：

- 信令协议和 WebRTC 里有大量业务/控制逻辑，目前每支持一个新平台就得重新实现一遍。
- 媒体设备的交互、编解码都跟具体平台和框架绑定。
- 对 Unity、Flutter、React Native 这类多平台框架来说，上面这两件事极其痛苦。

所以我们想：做一套 Rust SDK（本来也打算做），把所有业务逻辑和平台相关 API 收进一组干净抽象里，它同时还能当我们其它 SDK 的地基 —— 于是就动手了。

我们会先把它作为 Unity SDK（开发中）的基础，往后它会成为其它 SDK 的底座。

## 本 fork 的定制说明（非上游内容）

本仓库是 [livekit/rust-sdks](https://github.com/livekit/rust-sdks) 的**定制分支**，用途是给公司会议客户端（C#/Avalonia，通过 `livekit-ffi` 走 FFI）提供原生库与"省 CPU"的视频直出能力。上游代码全部保留，定制只动了下述几处 —— 完整差异：

```bash
git diff upstream/main...custom/d3d11-video-sink     # 21 个文件，+2587 / -18
```

### 1. 视频帧直出（原生 VideoSink，省 CPU 的关键）

- **Windows**：`webrtc-sys/src/d3d11_video_sink.{h,cpp,rs}` —— 解码后的帧不再转成 BGRA 再逐帧拷给上层，而是以 **D3D11 共享纹理**（NT handle / GlobalSharedHandle，配 keyed mutex 同步）直接交给渲染侧。
- **macOS**：`webrtc-sys/src/iosurface_video_sink.{h,cpp,rs}` —— 走 **IOSurface** 共享。
- FFI 侧新增纹理帧事件：`livekit-ffi/protocol/video_frame.proto`、`livekit-ffi/src/server/video_stream.rs`、`livekit-ffi/src/conversion/video_frame.rs`、`libwebrtc/src/video_stream.rs`。
- 默认**不开启**，由上层按需打开；打开后上层拿到的是"共享纹理句柄 + 同步信息"，不需要再拷一次像素。

### 2. 判锁与资源释放的修复

- `webrtc-sys/src/d3d11_video_sink.cpp`：`WaitForSingleObject` 返回 `WAIT_TIMEOUT(0x102)` 曾被 `SUCCEEDED()` 误判成成功（消费端会去读还没准备好的纹理）；同时**只在终态交还 keyed mutex 钥匙**。
- `livekit/src/room/participant/local_participant.rs`：取消发布视频轨后补 `transceiver.stop()` —— 之前反复开关屏幕共享/摄像头，每轮都会漏一套 VideoSendStream + EncoderQueue 线程，内存只涨不降（要关掉会议才释放）。

### 3. 音频：设备热切换与回声/降噪收敛

- `livekit-ffi/src/server/platform_audio.rs`、`livekit/src/platform_audio/mod.rs`：采集/放音设备改走 `switch_*`，**会议中可热切换麦克风/扬声器**，不用退会重进。
- ADM 的 `prefer_hardware` 默认保守（走软件 AEC3）；硬件 AEC/NS 不可用时**自动回退**，参数支持运行中应用。
- `livekit/src/room/track/mod.rs`：`set_muted` 永远对齐到媒体层（不再因为"元数据已等于目标值"就提前 return）。

### 4. 构建与 CI

- `.github/workflows/custom-native-builds.yml`：手动触发（`workflow_dispatch`），产出 **`livekit_ffi.dll`（Windows x64）** 与 **`liblivekit_ffi.dylib`（macOS arm64 / x64）** —— 上层 C# SDK 里的原生库就来自这里。
- `.github/matchers/clang.json`：把 C++ 编译错误转成 GitHub 注解；构建失败时还会把 `build.log` 尾巴贴成 commit 评论，方便直接定位。

### 5. 解码后真实帧尺寸 + 关键帧请求（观看端首帧 / 竖横屏切换）

- **共享纹理里多写两个槽位**：`webrtc-sys/src/d3d11_video_sink.cpp` 的帧计数共享内存从 5 个 LONG 扩到 **7 个**，
  `slot 5/6 = 解码后的真实帧宽高`（每写成一帧更新，且在帧号 +1 之前发布）。
  纹理宽高在建流时就定死了，源换方向（手机竖屏 ↔ 横屏）时原生只能把新画面**等比塞进旧纹理并居中留黑**，
  上层再按纹理比例摆放就是"中间一条小画面"。有了这两个槽，观看侧（`GpuTexturePresenter`）能立刻发现
  "纹理比例 ≠ 源比例"并**按新尺寸重建纹理**。冒烟用例：`test_read_frame_size`。
- **macOS 同款**：`webrtc-sys/src/iosurface_video_sink.cpp` 的计数器文件 `/tmp/livekit_gpu_frame_<句柄>` 也从
  4 个 int32 扩到 **7 个**，同样在帧号 +1 之前写 `slot 5/6`。C# 侧 `GpuTexturePresenter` 是同一套读法
  （按 7/5/4/2 逐级退让打开映射），所以两边行为一致。冒烟用例：`test_read_frame_size_iosurface`。
- **主动请求关键帧**：`webrtc-sys` 的 `VideoTrack::request_refresh_frame()` 走
  **`track()->GetSource()->GenerateKeyFrame()`**（远端轨 = `VideoRtpTrackSource`，它只实现了
  `GenerateKeyFrame()`，会经 `VideoRtpReceiver` → `VideoReceiveStream` 向发送端发 **RTCP PLI/FIR**），
  再补一次 `RequestRefreshFrame()`（本地摄像头预览走这条）；
  ⚠️ **别只调 `RequestRefreshFrame()`**：在远端源上它落到 `VideoSourceInterface` 的**空实现**，
  调了等于没调（2026-10-20 现场：连发 6 次"请求"，`到 sink ≈ 0 帧`，人一直看灰屏）；
  再往上依次是 `libwebrtc::RtcVideoTrack::request_refresh_frame()`、
  `livekit::RemoteVideoTrack::request_key_frame()`、`livekit::Track::request_key_frame()`，
  最后在 `livekit-ffi/src/cabi.rs` 导出 C ABI：

  ```c
  bool livekit_video_track_request_keyframe(uint64_t track_handle);
  ```

  场景：订阅端入会 / 重建纹理时，发送端可能正落在两个关键帧之间；画面完全静止（PPT、白板、不动的桌面）
  时它可能几十秒都不产关键帧，本端解码器一帧都出不来（现场：入会后一直停在头像墙，直到对方动一下画面）。
  旧版本没有这个导出，语言绑定按 `EntryPointNotFoundException` 降级即可。
  **平台无关**：导出在 `livekit-ffi/src/cabi.rs`，`VideoTrack::request_refresh_frame()` 在共享 C++ 里，
  Windows / macOS / Linux 一样可用（各自的产物都要重新编译才带得动）。
- `webrtc-sys/build.rs`：Windows 分支补 `/utf-8`。源码里有大量中文注释，MSVC 默认按系统代码页（GBK）解析时，
  某些 UTF-8 字节序列会被拼成"行尾反斜杠"把下一行吞进注释，报出来的却是几十行之后"某某未声明"。

### 升级上游

定制都在 `custom/*` 分支上，`main` 只跟随 `upstream/main`。升级步骤见主仓 `tools/native-d3d11-sink/UPGRADE.md`（配 `setup-custom-repo.ps1` 可一键把定制补丁打到新的上游 tag 上）。

<!--BEGIN_REPO_NAV-->
<br/><table>
<thead><tr><th colspan="2">LiveKit 生态</th></tr></thead>
<tbody>
<tr><td>Agents SDK</td><td><a href="https://github.com/livekit/agents">Python</a> · <a href="https://github.com/livekit/agents-js">Node.js</a></td></tr><tr></tr>
<tr><td>LiveKit 客户端 SDK</td><td><a href="https://github.com/livekit/client-sdk-js">Browser</a> · <a href="https://github.com/livekit/client-sdk-swift">Swift</a> · <a href="https://github.com/livekit/client-sdk-android">Android</a> · <a href="https://github.com/livekit/client-sdk-flutter">Flutter</a> · <a href="https://github.com/livekit/client-sdk-react-native">React Native</a> · <b>Rust</b> · <a href="https://github.com/livekit/node-sdks">Node.js</a> · <a href="https://github.com/livekit/python-sdks">Python</a> · <a href="https://github.com/livekit/client-sdk-unity">Unity</a> · <a href="https://github.com/livekit/client-sdk-unity-web">Unity (WebGL)</a> · <a href="https://github.com/livekit/client-sdk-esp32">ESP32</a> · <a href="https://github.com/livekit/client-sdk-cpp">C++</a></td></tr><tr></tr>
<tr><td>起步示例应用</td><td><a href="https://github.com/livekit-examples/agent-starter-python">Python Agent</a> · <a href="https://github.com/livekit-examples/agent-starter-node">TypeScript Agent</a> · <a href="https://github.com/livekit-examples/agent-starter-react">React App</a> · <a href="https://github.com/livekit-examples/agent-starter-swift">SwiftUI App</a> · <a href="https://github.com/livekit-examples/agent-starter-android">Android App</a> · <a href="https://github.com/livekit-examples/agent-starter-flutter">Flutter App</a> · <a href="https://github.com/livekit-examples/agent-starter-react-native">React Native App</a> · <a href="https://github.com/livekit-examples/agent-starter-embed">Web Embed</a></td></tr><tr></tr>
<tr><td>UI 组件</td><td><a href="https://github.com/livekit/components-js">React</a> · <a href="https://github.com/livekit/components-android">Android Compose</a> · <a href="https://github.com/livekit/components-swift">SwiftUI</a> · <a href="https://github.com/livekit/components-flutter">Flutter</a></td></tr><tr></tr>
<tr><td>服务端 API</td><td><a href="https://github.com/livekit/node-sdks">Node.js</a> · <a href="https://github.com/livekit/server-sdk-go">Golang</a> · <a href="https://github.com/livekit/server-sdk-ruby">Ruby</a> · <a href="https://github.com/livekit/server-sdk-kotlin">Java/Kotlin</a> · <a href="https://github.com/livekit/python-sdks">Python</a> · <b>Rust</b> · <a href="https://github.com/agence104/livekit-server-sdk-php">PHP (community)</a> · <a href="https://github.com/pabloFuente/livekit-server-sdk-dotnet">.NET (community)</a></td></tr><tr></tr>
<tr><td>资源</td><td><a href="https://docs.livekit.io">Docs</a> · <a href="https://docs.livekit.io/mcp">Docs MCP Server</a> · <a href="https://github.com/livekit/livekit-cli">CLI</a> · <a href="https://cloud.livekit.io">LiveKit Cloud</a></td></tr><tr></tr>
<tr><td>LiveKit 服务端（开源）</td><td><a href="https://github.com/livekit/livekit">LiveKit server</a> · <a href="https://github.com/livekit/egress">Egress</a> · <a href="https://github.com/livekit/ingress">Ingress</a> · <a href="https://github.com/livekit/sip">SIP</a></td></tr><tr></tr>
<tr><td>社区</td><td><a href="https://community.livekit.io">Developer Community</a> · <a href="https://livekit.io/join-slack">Slack</a> · <a href="https://x.com/livekit">X</a> · <a href="https://www.youtube.com/@livekit_io">YouTube</a></td></tr>
</tbody>
</table>
<!--END_REPO_NAV-->