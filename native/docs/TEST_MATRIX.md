# Test Matrix

This document is the ownership map for VoidPlayer tests. It classifies native
and UI tests into gates and records cleanup decisions that changed the active set.

## Gate Levels

| Gate | Purpose | Typical command |
| --- | --- | --- |
| PR fast | Stable, high-signal checks for shared native logic and platform backend canaries. | `python3.12 dev.py gate pr-fast` |
| Windows preservation | Windows runner/D3D11 preservation after shared renderer/backend changes. | `python dev.py gate windows-preservation` |
| macOS stabilization | macOS native playback and renderer-owned Metal confidence. | `python3.12 dev.py gate macos-ui-smoke` |
| Nightly/headed | Slower headed UI, stress, audio, 4K/cadence, and lifecycle churn. | `python3.12 dev.py gate macos-ui-nightly` or Windows UI suites |
| Release candidate | Full native config matrix, platform UI preservation, package, compliance, signing inputs. | `python3.12 dev.py gate release-candidate` plus CI full matrix |
| Manual/local | Tests needing local media, audible speakers, external paths, or long perf runs. | `ui_tests/local/**`, manual audio/perf scripts |

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
ctest --test-dir native/build-macos-make -L contract --output-on-failure
ctest --test-dir native/build-macos-make -L macos --output-on-failure
ctest --test-dir native/build-macos-make -L backend --output-on-failure
ctest --test-dir native/build-macos-make -LE hosted-flaky --output-on-failure
```

## Native Test Inventory

| Test | Type | Gate | Requires | Covered risk |
| --- | --- | --- | --- | --- |
| `video_renderer_tests` | contract + integration | PR fast / Windows preservation | Windows FFmpeg | Shared renderer, D3D11, FFI command policy, decode/seek/layout unit coverage. |
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
| `presentation_loop_driver_smoke` | contract | PR fast | macOS native build | Presentation loop driver behavior. |
| `presentation_carry_forward_smoke` | contract | PR fast | FFmpeg media support | Frame carry-forward behavior. |
| `software_frame_queue_smoke` | contract | PR fast | FFmpeg media support | Software frame queue behavior. |
| `audio_mixer_smoke` | contract | PR fast | FFmpeg audio support | Mixer active-track behavior. |
| `macos_presentation_adapter_smoke` | backend canary | PR fast | macOS native build | Software fallback/parity adapter. |
| `macos_metal_uploader_smoke` | backend canary | PR fast | Metal, sample media | Metal uploader and CVPixelBuffer validation. |
| `macos_metal_presentation_backend_smoke` | backend canary | PR fast | Metal | Metal backend provider/draw canary. |
| `videotoolbox_provider_smoke` | backend canary | PR fast / macOS stabilization | VideoToolbox availability | VT provider support/fallback visibility. |
| `renderer_metal_headless_smoke` | backend integration | Nightly/headed or targeted Metal changes | Metal, sample media | Renderer-owned Metal headless path; labelled `hosted-flaky;nightly` and excluded from hosted PR fast because CI GPUs may fail visible-frame capture. |
| `macos_media_smoke` | native integration | macOS stabilization | FFmpeg media | macOS media open/metadata path. |
| `software_decode_frame_queue_smoke` | native integration | macOS stabilization | sample media | Software decode frame queue. |
| `decode_thread_software_smoke` | native integration | macOS stabilization | sample media | Decode thread software path. |
| `macos_native_player_shared_renderer_smoke` | native integration | macOS stabilization | Metal, sample media | Shared native player + renderer-owned presentation. |
| `macos_analysis_ffi_smoke` | analysis/FFI canary | macOS analysis changes | analysis build | macOS analysis symbols/cache path. |
| `macos_crash_handler_smoke` | diagnostics canary | macOS runner/native changes | macOS native build | Crash handler log contract. |

## Runner/UI Smoke Inventory

| Script group | Canonical role | Gate |
| --- | --- | --- |
| `ui_tests/smoke/basic.csv` | Windows runner and basic playback smoke. | Windows preservation / release candidate |
| `ui_tests/timeline/**` | Real pointer timeline/seek path. | Targeted Windows preservation; stress scripts nightly/release. |
| `ui_tests/seek/**` | Direct seek/step/rapid seek regressions. | Targeted preservation; rapid/storm scripts nightly. |
| `ui_tests/loop/**` | Loop range state and commit behavior. | Targeted preservation. |
| `ui_tests/viewport/**` | Pan/zoom/split/fullscreen layout. | Targeted preservation. |
| `ui_tests/track/**` | Add/remove/reorder/offset/network track behavior. | Targeted preservation. |
| `ui_tests/codec/**` | Codec-specific not-black/decode canaries. | Targeted preservation / release candidate. |
| `ui_tests/analysis/**` | Windows analysis UI/IPC and overlay. | Analysis changes / release candidate. |
| `ui_tests/color/**` | Color metadata/capture parity. | Release candidate or color pipeline changes. |
| `ui_tests/macos/native_facade_smoke.csv` | macOS channel/metadata/diagnostics smoke. | macOS stabilization PR gate candidate. |
| `ui_tests/macos/native_seek_frame_smoke.csv` | Renderer-owned refresh after seek. | macOS stabilization. |
| `ui_tests/macos/native_layout_split_smoke.csv` | Shared layout through Metal presentation. | macOS stabilization. |
| `ui_tests/macos/native_controls_smoke.csv` | Basic native play/pause/seek/step command smoke. | macOS stabilization. |
| `ui_tests/macos/native_4k60_playback_smoke.csv` | VideoToolbox/Metal/cadence canary. | Nightly/headed or release candidate. |
| `ui_tests/macos/native_vvc_software_playback_smoke.csv` | Software fallback + Metal package path. | Nightly/headed or release candidate. |
| `ui_tests/macos/native_audio_play_seek_smoke.csv` | Native audio play/seek and audible-track diagnostics. | Nightly/headed; manual audible check remains separate. |
| `ui_tests/macos/native_callback_stress_smoke.csv` | Callback lifecycle stress. | Nightly/headed. |
| `ui_tests/macos/native_quit_while_playing_smoke.csv` / `native_user_window_close_smoke.csv` | Teardown and crash-report regression. | Nightly/headed or targeted runner changes. |
| `ui_tests/local/**` | Developer-specific absolute-path regressions. | Manual/local only; never CI. |

## Workflow Mapping

| Workflow | Trigger | Gate |
| --- | --- | --- |
| `.github/workflows/native.yml` | push / PR | native PR fast, macOS native fast, macOS runner build, macOS analysis smoke |
| `.github/workflows/native.yml` | weekly or manual `full_matrix=true` | full Windows native config matrix |
| `.github/workflows/macos-ui.yml` | weekly | `python3.12 dev.py gate macos-ui-smoke` |
| `.github/workflows/macos-ui.yml` | manual `profile=macos-ui-smoke` or `macos-ui-nightly` | headed macOS UI smoke/nightly gate |

The local `release-candidate` gate and the GitHub full native config matrix are separate pieces of release evidence:
run both when preparing a release candidate.

## Phase 2 Cleanup Decisions

| Test or group | Decision | Reason |
| --- | --- | --- |
| `native_first_frame_smoke.csv` | Deleted. | Covered by `native_facade_smoke.csv` and `native_seek_frame_smoke.csv`. |
| `native_playback_smoke.csv` | Deleted. | Playback advancement is covered by 4K60/VVC and playing seek/step canaries. |
| `synthetic_texture_smoke.csv` | Deleted. | Early MVP synthetic texture path; renderer-owned Metal is the normal route. |
| `native_audio_diagnostics_smoke.csv` | Merged into `native_audio_play_seek_smoke.csv`, then deleted. | Keeps audible-track diagnostics with the audio play/seek smoke. |
| `native_frame_callback_lifecycle_smoke.csv` | Deleted. | `native_callback_stress_smoke.csv` remains the stronger nightly callback churn gate. |
| 4K60/VVC/P010 macOS UI smokes | Keep as release/nightly canaries unless a PR touches codec/color/backend. | High value but heavier than channel/seek/layout smokes. |
| Windows timeline exact seek resource scripts across codecs | Keep targeted, consider release matrix grouping. | Similar resource-risk shape across codecs. |
| CTest names ending in `_smoke` that are contract tests | Rename later only if churn is acceptable. | Labels now carry intent without breaking references. |

## Rules

- Do not add `ui_tests/local/**` to CI.
- Do not use headed UI stress/perf tests as default PR fast gates.
- Do not remove a smoke until this matrix records its covered risk and overlap.
- Prefer adding labels and gate docs before adding new one-off smoke scripts.
