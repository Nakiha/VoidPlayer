# Test Matrix

This document is the ownership map for VoidPlayer tests. It classifies native
and UI tests into gates and records cleanup decisions that changed the active set.

## Gate Levels

| Gate | Purpose | Typical command |
| --- | --- | --- |
| PR fast | Stable, high-signal checks for shared native logic and platform backend canaries. | `python3.12 dev.py gate pr-fast` |
| Platform protection | Cheap static guardrails for pinned Flutter fork, macOS presentation, and Windows compositor protection lines. | `python3.12 dev.py gate repo-hygiene` or targeted `*-protection` gates |
| Windows preservation | Windows runner/D3D11 preservation after shared renderer/backend changes. | `python dev.py gate windows-preservation` |
| macOS stabilization | macOS native playback and renderer-owned Metal confidence. | `python3.12 dev.py gate macos-ui-smoke` |
| macOS HDR EDR | Local HLG/PQ Auto promotion evidence on an EDR-capable display. | `python3.12 dev.py gate macos-hdr-edr-smoke` |
| macOS wgpu-metal canary | Experimental wgpu-metal software, VideoToolbox, source-projection/overlay, and 4K60 evidence. | `python3.12 dev.py gate macos-wgpu-metal-smoke` |
| macOS wgpu-metal EDR | Experimental wgpu-metal HLG/BT.2020 EDR evidence on an EDR-capable display. | `python3.12 dev.py gate macos-wgpu-metal-edr-smoke` |
| Nightly/headed | Slower headed UI, stress, audio, 4K/cadence, and lifecycle churn. | `python3.12 dev.py gate macos-ui-nightly` or Windows UI suites |
| macOS release readiness | macOS package stage, FFmpeg dylibs, `@rpath`, notices, entitlements, sandbox/crash-log inputs, and codesign smoke. | `python3.12 dev.py gate macos-release-readiness` |
| Release candidate | Full native config matrix, platform UI preservation, package, compliance, signing inputs. | `python3.12 dev.py gate release-candidate` plus CI full matrix |
| Manual/local | Tests needing local media, audible speakers, external paths, EDR display headroom, or long perf runs. | `ui_tests/local/**`, `macos-hdr-edr-smoke`, manual audio/perf scripts |

## CTest Labels

Native CTest labels are additive. Use labels to select intent, not to rename
targets:

| Label | Meaning |
| --- | --- |
| `contract` | Pure or mostly platform-neutral behavior contract. |
| `backend` | Platform presentation/decode backend canary. |
| `integration` | A real native playback or subsystem vertical slice. |
| `windows` / `macos` | Platform-specific native coverage. |
| `analysis` / `ffi` / `cli` | Analysis, C ABI, or CLI-specific coverage. |
| `videotoolbox` | VideoToolbox-specific native canary. |
| `diagnostics` | Crash/log/diagnostic behavior. |
| `audio` | Native audio behavior. |
| `hosted-flaky` | Valid targeted/nightly coverage, but excluded from hosted PR fast CI. |
| `nightly` | Belongs in nightly/headed or targeted runs rather than the PR fast gate. |

Examples:

```bash
ctest --test-dir build/native/standalone/macos-make -L contract --output-on-failure
ctest --test-dir build/native/standalone/macos-make -L macos --output-on-failure
ctest --test-dir build/native/standalone/macos-make -L backend --output-on-failure
ctest --test-dir build/native/standalone/macos-make -LE hosted-flaky --output-on-failure
```

## Native Test Inventory

| Test | Type | Gate | Requires | Covered risk |
| --- | --- | --- | --- | --- |
| `video_renderer_tests` | contract + integration | PR fast / Windows preservation | Windows FFmpeg | Shared renderer, D3D11, FFI command policy, decode/seek/layout unit coverage. |
| `video_renderer_tests [windows_display]` | platform contract | Windows PR fast / Windows preservation | Windows SDK/DXGI | Active-output selection, negative-coordinate layouts, deterministic fallback, and HDR color-space classification. |
| `video_renderer_tests [windows_cross_adapter]` | platform contract | Windows PR fast / Windows preservation | Windows SDK/D3D11 | LUID comparison, cross-adapter requirement decisions, event-query/shared-fence sync policy, safe fallback, and deterministic transport capability diagnostics without requiring a multi-GPU desktop. |
| `video_renderer_tests [windows_device_recovery]` | platform contract | Windows PR fast / Windows preservation | Windows SDK/DXGI | Device-loss state names, narrow DXGI removed/reset/hung classification, removed-reason handling, and preserved-player diagnostics contract. |
| `video_renderer_tests [windows_high_refresh]` | platform contract | Windows PR fast / Windows preservation | none beyond Windows build | High-refresh metric histograms, low-refresh functional-only classification, concrete hot-path failure reasons, drop-rate math, source-cache reuse, and source-projection redraw failure classification. |
| `video_renderer_tests [windows_overlay_layer]` | platform contract | Windows PR fast / Windows preservation | none beyond Windows build | Retained overlay signature, dirty rebuild, reuse, generation commit, fallback, and reset behavior. |
| `windows_d3d11_color_layout_parity_smoke` | backend contract | PR fast / Windows preservation | D3D11 hardware adapter; explicit WARP allowed only for hosted CI | Synthetic shared renderer snapshots through D3D11 capture; BGRA, NV12, planar YUV420, P010, odd stride, split/layout fit. |
| `windows_d3d11_fp16_scrgb_smoke` | backend contract | PR fast / Windows preservation | D3D11 hardware adapter; explicit WARP allowed only for hosted CI | RGBA16F scRGB capture, SDR white scaling, PQ/HLG/P010/BT.2020, unclipped highlights, layout/overlay pass, and BGRA source-rerender compatibility. |
| `video_renderer_tests [windows_dcomp]` | backend contract | PR fast / Windows preservation | D3D11 + keyed mutex | Native-compositor policy and shared FP16 ring lease/resize generations. |
| `video_renderer_tests [windows_source_cache]` | backend contract | PR fast / Windows preservation | D3D11 + keyed mutex | Atomic bundle publication, lease retirement, backpressure, signature replacement, and 384 MiB depth policy. |
| `video_renderer_tests [windows_source_projection]` | projection contract | PR fast / Windows preservation | none beyond Windows build | Four-track order, split, pan/zoom, negative/out-of-range UV, missing slots, and background fallback. |
| `windows_d3d11_source_projection_smoke` | backend contract | PR fast / Windows preservation | D3D11 hardware adapter; WARP accepted for hosted contract coverage | Real DComp projection shader, four source textures, deterministic order, and unclipped FP16 scRGB highlights. |
| `windows_d3d11_dcomp_flutter_composite_smoke` | backend contract | PR fast / Windows preservation | D3D11 hardware adapter; WARP accepted for hosted contract coverage | Executes the final shader against FP16 scRGB and BGRA8 SDR targets, including Flutter premultiplied-alpha composition. |
| `windows_d3d11_high_refresh_projection_overlay_smoke` | backend contract | PR fast / Windows preservation | none beyond Windows build | High-refresh projection/overlay gate canary, including pass/fail threshold classification and stale viewport redraw detection. |
| `windows_d3d11_retained_overlay_layer_smoke` | backend contract | PR fast / Windows preservation | none beyond Windows build | Retained overlay canary: dirty primitive packages rebuild once, projection ticks reuse the GPU buffer, and high-refresh gates reject raster-without-reuse. |
| `analysis_tests` | contract | Release candidate / analysis changes | analysis submodules/tools | VAC2/VACHUNK/cache and analysis FFI behavior. |
| `test_ffi_c` | FFI canary | PR fast when FFI is built | Windows FFI target | C ABI load/call sanity. |
| `voidplayer_cli_help` | CLI canary | Release candidate | analysis build | CLI starts and exposes help. |
| `analysis_cli_smoke` | CLI integration | Release candidate / analysis changes | analyzer + sample media | CLI end-to-end analysis smoke. |
| `software_bgra_converter_smoke` | contract | PR fast | macOS native build | BGRA copy/channel/stride behavior. |
| `software_frame_packer_smoke` | contract | PR fast | macOS native build | Software frame package layout. |
| `bgra_capture_metrics_smoke` | contract | PR fast | macOS native build | BGRA capture/hash/luma metrics. |
| `decoded_frame_sink_smoke` | contract | PR fast | macOS native build | Decoded frame sink contract. |
| `layout_geometry_smoke` | contract | PR fast | macOS native build | Shared layout geometry. |
| `renderer_config_validation_smoke` | contract | PR fast | macOS native build | Renderer config validation. |
| `presentation_snapshot_smoke` | contract | PR fast | macOS native build | Draw snapshot contract. |
| `presentation_carry_forward_smoke` | contract | PR fast | FFmpeg media support | Frame carry-forward behavior. |
| `software_frame_queue_smoke` | contract | PR fast | FFmpeg media support | Software frame queue behavior. |
| `audio_mixer_smoke` | contract | PR fast | FFmpeg audio support | Mixer active-track behavior. |
| `macos_presentation_adapter_smoke` | backend canary | PR fast | macOS native build | Software fallback/parity adapter. |
| `macos_metal_uploader_smoke` | backend canary | PR fast | Metal, sample media | Metal uploader and CVPixelBuffer validation. |
| `macos_metal_presentation_backend_smoke` | backend canary | PR fast | Metal | Metal backend provider/draw canary. |
| `macos_metal_color_layout_parity_smoke` | backend contract | PR fast | Metal | Synthetic shared renderer snapshots through Metal backend capture; BGRA, NV12, planar YUV420, P010, odd stride, split/layout fit. |
| `videotoolbox_provider_smoke` | backend canary | PR fast / macOS stabilization | VideoToolbox availability | VT provider support/fallback visibility. |
| `renderer_metal_headless_smoke` | backend integration | Nightly/headed or targeted Metal changes | Metal, sample media | Renderer-owned Metal headless path; labelled `hosted-flaky;nightly` and excluded from hosted PR fast because CI GPUs may fail visible-frame capture. |
| `macos_media_smoke` | native integration | macOS stabilization | FFmpeg media | macOS media open/metadata path. |
| `software_decode_frame_queue_smoke` | native integration | macOS stabilization | sample media | Software decode frame queue. |
| `decode_thread_software_smoke` | native integration | macOS stabilization | sample media | Decode thread software path. |
| `macos_native_player_shared_renderer_smoke` | native integration | macOS stabilization | Metal, sample media | Shared native player + renderer-owned presentation. |
| `macos_analysis_ffi_smoke` | analysis/FFI canary | macOS analysis changes | analysis build | macOS analysis symbols/cache path. |
| `analysis_generation_service_smoke` | analysis contract | macOS analysis changes / PR fast when analysis is enabled | analysis build | Resident native analysis worker service de-duplication, priority ordering, polling completion, and stats. |
| `macos_analysis_toolchain_smoke` | analysis CLI integration | macOS analysis changes | macOS analyzer, sample media | Portable `VoidPlayerCli` generates VAC2 base and analyzer-backed overlay VACHUNK, then reopens both. |
| `macos_crash_handler_smoke` | diagnostics canary | macOS runner/native changes | macOS native build | Crash handler log contract. |

## Runner/UI Smoke Inventory

| Script group | Canonical role | Gate |
| --- | --- | --- |
| `ui_tests/smoke/basic.csv` | Windows runner and basic playback smoke. | Windows preservation / release candidate |
| `ui_tests/smoke/native_seek_preview_event.csv` | Windows EventChannel, fixed SDR presentation, and active DXGI output diagnostics contract. | Windows preservation / release candidate |
| `ui_tests/smoke/native_seek_preview_event_dcomp_scrgb.csv` | Locked-engine Flutter export, DComp activation/ACK, seek, resize, viewport alpha, and fail-closed compositor failure. | Windows preservation / targeted presentation changes |
| `ui_tests/smoke/native_compositor_flutter_surface_pump_scrgb.csv` | Locked-engine compositor-owned Flutter surface pump for settings, media-info, toolbar hover, export publish/request diagnostics, and stale-generation fail-closed coverage. | Windows preservation / targeted presentation changes |
| `ui_tests/smoke/native_compositor_window_close_scrgb.csv` | Windows native compositor close path through `WindowManager.close`, ensuring the main window hides before native/DComp teardown so no transparent viewport/black-frame flash is shown. | Windows preservation / targeted presentation changes |
| `ui_tests/smoke/native_source_projection_dcomp_scrgb.csv` | Paused/playing source cache, pan/zoom, split, analysis overlay projection, seek/resize, diagnostics, and fail-closed cleanup. | Windows preservation / targeted presentation changes |
| `ui_tests/smoke/native_compositor_auto_sdr.csv` | Default Windows Auto policy selects the BGRA8 SDR native compositor and locks nominal 80-nit white. | Windows preservation |
| `ui_tests/smoke/native_compositor_device_recovery_sdr.csv` | Debug-injected presentation device-loss recovery in default Auto/SDR mode; verifies player/track preservation, counters, active compositor, seek, and EventChannel health. | Windows preservation / targeted device recovery changes |
| `ui_tests/smoke/native_compositor_device_recovery_scrgb.csv` | Debug-injected DComp/compositor device-loss recovery in forced scRGB mode; verifies FP16 target returns active and pan/seek survive. | Windows preservation / targeted device recovery changes |
| `ui_tests/smoke/native_source_projection_device_recovery.csv` | Debug-injected source-cache device-loss recovery while source projection is active; verifies cache republish, pan/zoom, split, playing-state seek presented anchor, and EventChannel health. | Windows preservation / targeted device recovery changes |
| `ui_tests/smoke/native_high_refresh_paused_pan_zoom.csv` | Forced scRGB paused dual-track pan/zoom; verifies source-projection hot path, source reuse, and zero viewport redraws. | Windows preservation / `windows-high-refresh-local` |
| `ui_tests/smoke/native_high_refresh_playing_pan_split.csv` | Forced scRGB playing pan/split/order; verifies DComp remains active, source reuse grows, and seek still works. | Windows preservation / `windows-high-refresh-local` |
| `ui_tests/smoke/native_high_refresh_overlay_pan_zoom.csv` | Forced scRGB source-projection overlay interaction; verifies retained overlay layer active, reuse/composite counters, no viewport redraw during projection, and visible output. | Windows preservation / `windows-high-refresh-local` |
| `ui_tests/smoke/windows_hdr_auto_runtime.csv` | Adds/removes an HLG track and verifies live SDR/scRGB promotion/demotion, white-level, generations, and playback continuity. | Local `windows-hdr-auto`; requires Windows HDR enabled |
| `ui_tests/timeline/**` | Real pointer timeline/seek path. | Targeted Windows preservation; stress scripts nightly/release. |
| `ui_tests/seek/**` | Direct seek/step/rapid seek regressions. | Targeted preservation; rapid/storm scripts nightly. |
| `ui_tests/loop/**` | Loop range state and commit behavior. | Targeted preservation. |
| `ui_tests/viewport/**` | Pan/zoom/split/fullscreen layout. | Targeted preservation. |
| `ui_tests/track/**` | Add/remove/reorder/offset/network track behavior. | Targeted preservation. |
| `ui_tests/codec/**` | Codec-specific not-black/decode canaries. | Targeted preservation / release candidate. |
| `ui_tests/analysis/**` | Windows analysis UI/IPC plus cross-platform overlay activation/seek regressions. | Analysis changes / release candidate. |
| `ui_tests/color/**` | Color metadata/capture parity. | Release candidate or color pipeline changes. |
| `ui_tests/macos/native_facade_smoke.csv` | macOS channel/metadata/diagnostics smoke. | macOS stabilization PR gate candidate. |
| `ui_tests/macos/native_seek_frame_smoke.csv` | Renderer-owned refresh after seek. | macOS stabilization. |
| `ui_tests/macos/native_layout_split_smoke.csv` | Shared layout through Metal presentation. | macOS stabilization. |
| `ui_tests/macos/native_controls_smoke.csv` | Basic native play/pause/seek/step command smoke. | macOS stabilization. |
| `ui_tests/macos/native_compositor_auto_sdr_policy_smoke.csv` | Default Auto policy keeps SDR media on the SDR native-compositor target and avoids EDR layer promotion. | macOS stabilization. |
| `ui_tests/macos/native_compositor_auto_hlg_policy_smoke.csv` | Portable HLG fixture promotes Auto to the EDR compositor and verifies `64RGBAHalf` output above SDR reference white. | Local `macos-hdr-edr-smoke`; requires an EDR-capable display. |
| `ui_tests/macos/native_compositor_add_hlg_promotes_edr_smoke.csv` | Starts with SDR media, adds an HLG track at runtime, and verifies Auto policy rebuilds the compositor/renderer target as EDR `64RGBAHalf`. | Local `macos-hdr-edr-smoke`; requires an EDR-capable display. |
| `ui_tests/macos/native_remove_hlg_paused_pan_smoke.csv` | Removes an HLG track from a mixed SDR+HLG session while playing, then verifies the remaining SDR track still responds to paused source-projection pan. | Targeted HDR policy / source-ring / track removal regressions. |
| `ui_tests/macos/native_remove_first_hlg_paused_pan_smoke.csv` | Removes the first HLG track from a mixed HLG+SDR session, leaving the SDR track in its original native slot and verifying paused source projection stays active. | Targeted source-ring slot/fileId regressions after track removal. |
| `ui_tests/macos/native_media_header_remove_smoke.csv` | Real media-header remove button path for native fileId 0 and remaining-track presentation. | Targeted track/header changes; candidate for stabilization smoke after the layout smoke gate is stable. |
| `ui_tests/macos/analysis_gated_smoke.csv` | macOS analysis FFI, media-header overlay panel/activation, and gated external analysis window behavior. | Nightly/headed or targeted analysis overlay changes. |
| `ui_tests/analysis/overlay_seek_boundary_hevc_aq.csv` / `overlay_seek_boundary_vvc.csv` | Real timeline seek near VACHUNK window boundaries; validates async chunk readiness, native overlay rebinding, and redraw. | Targeted analysis overlay changes on Windows or macOS; use `mac-ui-test --build` for macOS renderer-owned Metal. |
| `ui_tests/macos/native_4k60_playback_smoke.csv` | VideoToolbox/Metal/cadence canary; asserts monotonic PTS, large-gap/error counters, duplicate PTS visibility, host interval max/p95, and renderer-owned ratio. | Nightly/headed or release candidate. |
| `ui_tests/macos/native_playing_dual_track_pan_smoke.csv` | Playing pan intent coalescing; asserts display-link viewport composite can reuse the source-frame cache while video source updates remain PTS-driven. | Targeted viewport/backend changes; candidate for macOS stabilization. |
| `ui_tests/macos/native_paused_dual_track_pan_zoom_smoke.csv` | Paused dual-track pan/zoom through display-link source-cache composite. | Targeted viewport/backend changes. |
| `ui_tests/macos/native_eof_remove_to_short_tail_smoke.csv` | Removes long tracks after EOF until only a shorter track remains, verifying the last stable displayed frame stays non-black. | Targeted EOF / track removal / renderer-owned texture bridge changes. |
| `ui_tests/macos/native_eof_seek_dual_track_layout_smoke.csv` | EOF source-cache composite, seek recovery, and dual-track layout visibility. | Targeted EOF/seek/layout changes. |
| `ui_tests/macos/native_vvc_software_playback_smoke.csv` | Software fallback + Metal package path. | Nightly/headed or release candidate. |
| `ui_tests/macos/native_add_short_after_eof_smoke.csv` | EOF carry-forward when adding a shorter hardware-decoded track after AV1/VVC software tracks. | Nightly/headed or targeted track/presentation changes. |
| `ui_tests/macos/native_audio_play_seek_smoke.csv` | Native audio play/seek and audible-track diagnostics. | Nightly/headed; manual audible check remains separate. |
| `ui_tests/macos/native_callback_stress_smoke.csv` | Callback lifecycle stress. | Nightly/headed. |
| `ui_tests/macos/native_quit_while_playing_smoke.csv` / `native_user_window_close_smoke.csv` | Teardown and crash-report regression. | Nightly/headed or targeted runner changes. |
| `ui_tests/macos/wgpu_metal_default_software_smoke.csv` | Experimental wgpu-metal software/package presentation canary; asserts backend selection, software fallback, YUV upload, and non-black capture. | Local `macos-wgpu-metal-smoke`. |
| `ui_tests/macos/wgpu_metal_videotoolbox_smoke.csv` | Experimental wgpu-metal VideoToolbox CVPixelBuffer source import canary. | Local `macos-wgpu-metal-smoke`; sets `VOIDPLAYER_WGPU_METAL_ENABLE_VIDEOTOOLBOX=1`. |
| `ui_tests/macos/wgpu_metal_source_projection_overlay_smoke.csv` | Experimental wgpu-metal retained source-projection and overlay composite canary through split/pan/zoom. | Local `macos-wgpu-metal-smoke`. |
| `ui_tests/macos/wgpu_metal_4k60_videotoolbox_smoke.csv` | Experimental wgpu-metal 4K60 VideoToolbox/cadence pressure canary. | Local `macos-wgpu-metal-smoke`; sets `VOIDPLAYER_WGPU_METAL_ENABLE_VIDEOTOOLBOX=1`. |
| `ui_tests/macos/wgpu_metal_hlg_edr_smoke.csv` | Experimental wgpu-metal HLG/BT.2020 EDR canary with RGBA16F target and renderer-owned color diagnostics. | Local `macos-wgpu-metal-edr-smoke`; requires an EDR-capable display. |
| `ui_tests/local/**` | Developer-specific absolute-path regressions. | Manual/local only; never CI. |

## Workflow Mapping

| Workflow | Trigger | Gate |
| --- | --- | --- |
| `.github/workflows/native.yml` | push / PR | native PR fast, macOS native fast, macOS runner build, macOS analysis smoke |
| `.github/workflows/native.yml` | weekly or manual `full_matrix=true` | full Windows native config matrix |
| `.github/workflows/native.yml` | manual `windows_ui_preservation=true` | GitHub-hosted Windows runner build + basic and native event/diagnostics UI smokes; skips analyzer tool bundling because the smokes do not cover analysis overlay, and enables `VOIDPLAYER_ALLOW_D3D11_HEADLESS_WARP_FALLBACK=1` because hosted Windows exposes only an unusable software DXGI adapter. Native coverage and the D3D11 color/layout canary stay in the same workflow's `Windows PR fast gate` job. |
| `.github/workflows/macos-ui.yml` | weekly | `python3.12 dev.py gate macos-ui-smoke` |
| `.github/workflows/macos-ui.yml` | manual `profile=macos-ui-smoke` or `macos-ui-nightly` | headed macOS UI smoke/nightly gate |
| Local HDR EDR gate | manual on EDR-capable macOS display before merging HDR compositor changes | `python3.12 dev.py gate macos-hdr-edr-smoke` |
| Local wgpu-metal gate | manual before promoting the experimental wgpu backend | `python3.12 dev.py gate macos-wgpu-metal-smoke` |
| Local wgpu-metal EDR gate | manual on EDR-capable macOS display before promoting wgpu-metal HDR/EDR | `python3.12 dev.py gate macos-wgpu-metal-edr-smoke` |
| Local Windows cross-adapter gate | manual on multi-adapter Windows desktop before merging output migration or shared-fence sync changes; runs event-query and shared-fence A/B evidence | `python dev.py gate windows-cross-adapter-local` |
| Local macOS package gate | manual before release candidate | `python3.12 dev.py gate macos-release-readiness` |

The local `release-candidate` gate and the GitHub full native config matrix are
separate pieces of release evidence: run both when preparing a release
candidate.

For the macOS HDR/native-compositor merge, treat these as the minimum merge
evidence set: `toolchain doctor`, macOS local-engine bootstrap, `pr-fast`,
`macos-ui-smoke`, local `macos-hdr-edr-smoke` on an EDR-capable display,
`macos-release-readiness`, Windows preservation, and green GitHub Flutter/Native
checks. The GitHub-hosted Windows UI preservation job may use documented
CI-only adapter fallbacks; do not treat that fallback as release coverage for a
real Windows desktop GPU.

For the experimental wgpu-metal backend, add local `macos-wgpu-metal-smoke` and,
on an EDR-capable display, `macos-wgpu-metal-edr-smoke` before discussing a
default-backend switch.

## Rules

- Do not add `ui_tests/local/**` to CI.
- Do not use headed UI stress/perf tests as default PR fast gates.
- Do not remove a smoke until this matrix records its covered risk and overlap.
- Prefer adding labels and gate docs before adding new one-off smoke scripts.
