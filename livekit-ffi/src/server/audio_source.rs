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

use std::{borrow::Cow, slice};

use livekit::webrtc::prelude::*;

use super::FfiHandle;
use crate::{proto, server, FfiError, FfiHandleId, FfiResult};

pub struct FfiAudioSource {
    pub handle_id: FfiHandleId,
    pub source_type: proto::AudioSourceType,
    pub source: RtcAudioSource,
}

impl FfiHandle for FfiAudioSource {}

impl FfiAudioSource {
    pub fn setup(
        server: &'static server::FfiServer,
        new_source: proto::NewAudioSourceRequest,
    ) -> FfiResult<proto::OwnedAudioSource> {
        let source_type = new_source.r#type();
        #[allow(unreachable_patterns)]
        let source_inner = match source_type {
            #[cfg(not(target_arch = "wasm32"))]
            proto::AudioSourceType::AudioSourceNative => {
                use livekit::webrtc::audio_source::native::NativeAudioSource;

                let audio_source = NativeAudioSource::new(
                    new_source.options.map(Into::into).unwrap_or_default(),
                    new_source.sample_rate.unwrap_or(48000),
                    new_source.num_channels.unwrap_or(1),
                    new_source.queue_size_ms.unwrap_or(1000),
                );
                RtcAudioSource::Native(audio_source)
            }
            #[cfg(not(target_arch = "wasm32"))]
            proto::AudioSourceType::AudioSourcePlatform => {
                // Platform ADM-based source - captures from microphone automatically
                // PlatformAudio must be created first to enable ADM recording

                // If options and platform_audio_handle are provided, configure audio processing
                if let (Some(ref options), Some(handle)) =
                    (&new_source.options, new_source.platform_audio_handle)
                {
                    if let Ok(ffi_audio) =
                        server.retrieve_handle::<super::platform_audio::FfiPlatformAudio>(handle)
                    {
                        // 【本地定制 fork】引擎侧音频处理参数：
                        //   · prefer_hardware 未显式给定时用**保守值 false**（软件 AEC3 / NS / AGC）。
                        //     为什么不是"hw_aec 可用就用硬件"：实测（2026-10-13，台式机 Realtek 驱动）
                        //     hw_aec 明明报 true，但**一旦真的启用硬件 AEC，随后的 start_recording 就会失败**
                        //     （先建源再 StartRecording 失败；停采集→应用→再起也失败）→ 采集直接静默。
                        //     要试硬件请显式写 Audio:NativePreferHardware=1。
                        //   · 能力探测值照打出来，便于判断设备到底声称支持什么。
                        let hw_aec = ffi_audio.audio.is_hardware_aec_available();
                        let hw_agc = ffi_audio.audio.is_hardware_agc_available();
                        let hw_ns = ffi_audio.audio.is_hardware_ns_available();
                        let prefer_hw = options.prefer_hardware.unwrap_or(false);
                        log::info!(
                            "[PLATFORM_AUDIO_FFI] audio processing: prefer_hw={} (explicit={:?}, hw_aec={}, hw_agc={}, hw_ns={})",
                            prefer_hw,
                            options.prefer_hardware,
                            hw_aec,
                            hw_agc,
                            hw_ns
                        );
                        let processing_options = livekit::AudioProcessingOptions {
                            echo_cancellation: options.echo_cancellation,
                            noise_suppression: options.noise_suppression,
                            auto_gain_control: options.auto_gain_control,
                            prefer_hardware_processing: prefer_hw,
                        };
                        if let Err(e) =
                            ffi_audio.audio.configure_audio_processing(processing_options)
                        {
                            log::warn!(
                                "Failed to configure audio processing for platform source: {}",
                                e
                            );
                        }
                        // 这里必须发生在 StartRecording 之前：Windows ADM 的
                        // EnableBuiltInAEC/AGC/NS 在录音初始化后调用必然失败。C# 侧现已先建源、
                        // 应用参数，再启动采集；dll 侧另有一道 recording_initialized 护栏。
                    }
                }

                RtcAudioSource::Device
            }
            _ => return Err(FfiError::InvalidRequest("unsupported audio source type".into())),
        };

        let handle_id = server.next_id();
        let source = Self { handle_id, source_type, source: source_inner };

        let info = proto::AudioSourceInfo::from(&source);
        server.store_handle(source.handle_id, source);

        Ok(proto::OwnedAudioSource { handle: proto::FfiOwnedHandle { id: handle_id }, info: info })
    }

    pub fn clear_buffer(&self) {
        match self.source {
            #[cfg(not(target_arch = "wasm32"))]
            RtcAudioSource::Native(ref source) => source.clear_buffer(),
            _ => {}
        }
    }

    pub fn capture_frame(
        &self,
        server: &'static server::FfiServer,
        capture: proto::CaptureAudioFrameRequest,
    ) -> FfiResult<proto::CaptureAudioFrameResponse> {
        let buffer = capture.buffer;

        let source = self.source.clone();
        let async_id = server.resolve_async_id(capture.request_async_id);

        let data = unsafe {
            let len = buffer.num_channels * buffer.samples_per_channel;
            slice::from_raw_parts(buffer.data_ptr as *const i16, len as usize)
        }
        .to_vec();

        // Use dedicated audio_runtime for audio capture
        let handle = server.audio_runtime.spawn(async move {
            // The data must be available as long as the client receive the callback.
            match source {
                #[cfg(not(target_arch = "wasm32"))]
                RtcAudioSource::Native(ref source) => {
                    let audio_frame = AudioFrame {
                        data: Cow::Owned(data),
                        sample_rate: buffer.sample_rate,
                        num_channels: buffer.num_channels,
                        samples_per_channel: buffer.samples_per_channel,
                    };

                    let res = source.capture_frame(&audio_frame).await;
                    if let Err(e) = server.send_event(
                        proto::CaptureAudioFrameCallback {
                            async_id,
                            error: res.err().map(|e| e.to_string()),
                        }
                        .into(),
                    ) {
                        log::error!(
                            "[AUDIO_CAPTURE] Failed to send callback async_id={}: {}",
                            async_id,
                            e
                        );
                    }
                }
                _ => {}
            }
        });
        server.watch_panic(handle);

        Ok(proto::CaptureAudioFrameResponse { async_id })
    }
}
