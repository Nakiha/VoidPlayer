# Native Stabilization History

本文件归档 2026-05-14/15 native 深度清理收敛轮的 patch 流水账、证据和验证结果。
当前执行入口请看 `native/docs/NATIVE_STABILIZATION_ROUND.md`。

来源：

- `build/chat/review_native.md`
- `build/chat/review_godobject.md`
- `build/chat/review_overlay.md`
- `build/chat/split_adv.md`
- 本轮对当前源码的静态核验

目标不是一次性拆完 `Renderer`，而是先收住最容易导致偶发崩溃、错帧、音频空洞、纹理覆盖和并发 UB 的边界。

## 工作规则

- 本轮不新增大功能。
- 本轮不扩大 Renderer public API，除非为了返回明确错误状态。
- 本轮不新增 process-global 状态。
- 本轮不新增文件格式。
- 本轮不改 Dart UI 交互，除非 native API 语义必须同步。
- 每个 patch 只处理一个 owner/lifetime/threading 问题。
- 每个 patch 完成后先测试，再更新本文档状态。
- 修改 native C++ 后至少运行 `python dev.py test --native-only`。
- 影响 Flutter runner、Texture、上屏、窗口交互或 `windows/runner/` 时，补跑带 `--build` 的相关 UI 脚本。

## 核验结论

chat 给出的方向和当前代码状态高度匹配。优先级最高的不是审美式拆分，而是 correctness / race / lifetime。

已核验为真实存在：

| ID | 问题 | 当前证据 | 风险 | 本轮状态 |
| --- | --- | --- | --- | --- |
| S1 | `DemuxThread::seek_callback_` 注册竞态 | `TrackPipelineManager::create_pipeline()` 先 `demux_thread->start()`，后 `set_seek_callback()`；`DemuxThread::run()` 并发读 callback | C++ data race / initial seek 玄学 | DONE - Patch 1 |
| S2 | paused audio 持续丢弃 PCM | `WaveOutOutput::render()` 在 `!playing_` 时写 silence 后调用 `discard_unheard(frames, kNoTrack, kNoTrack)` | pause/resume 音频空洞、underrun、重新对齐异常 | DONE - Patch 2 |
| S3 | `RenderSink` 长期保存裸 `TrackBuffer*` | `RenderSink::tracks_` 是裸指针数组；render loop `evaluate()` 与 remove/compact 不共享明确锁契约 | add/remove/compact 时 UAF 或错轨 | DONE - Patch 3 |
| S4 | Headless shared texture 没有 in-flight tracking | `pick_free_buffer()` 固定返回 `(front + 2) % 3`；release callback 只保证 lifetime，不保证内容不被重写 | Flutter 仍采样旧 texture 时 native 覆盖导致闪帧/撕裂 | DONE - Patch 4 |
| S5 | `AnalysisManager` session/global state 并发风险 | `loaded_ / vac2_base_ / analysis_path_` 无 session 级锁或 immutable snapshot；render thread 可同时读 overlay frame | render/FFI/load/unload 并发 UB 或错 chunk | DONE - Patch 5 |

第二梯队，确认存在但本轮可以排在前五项之后：

| ID | 问题 | 当前证据 | 建议时机 |
| --- | --- | --- | --- |
| S6 | `capture_front_buffer_locked()` 持 texture mutex 做 GPU copy/map | `Renderer::capture_front_buffer()` 同时持 `device_mutex_` 和 texture mutex 调 staging copy/map | DONE - Patch 6 |
| S7 | layout validation 太宽松 | `validate_layout_state()` 只检查 enum、finite、zoom positive | DONE - Patch 7 |
| S8 | `TextureManager::create_rgba_texture()` 缺尺寸校验 | RGBA create 直接 cast width/height，其他 create API 有基本校验 | DONE - Patch 8 |
| S9 | demux read error 没传播成明确 track error/event | `DemuxThread::run()` 非 EOF read error 后 break，最后只 `abort_outputs()` | DONE - Patch 9 |
| S10 | `avcodec_open2()` 未包 SEH guard | send/receive 已有 SEH wrapper，open 阶段仍直调 | DONE - Patch 10 |
| S11 | odd-dimension software path 直接拒绝 | `calculate_yuv420_layout()` 要求 width/height 都是偶数 | DONE - Patch 11 |
| S12 | `D3D11Device::shutdown()` 缺 `ClearState + Flush` | shutdown 直接 reset swapchain/context/device | DONE - Patch 12 |
| S13 | `NativePlayer` facade 直接转发 Renderer | `native_player.h` public methods inline 调 renderer，没有统一生命周期锁/状态门禁 | DONE - Patch 13 |
| S14 | FFI handle 长操作独占 per-player mutex | `ffi_exports.cpp` 的 `checked_player()` 持独占锁覆盖整个 native 调用 | DONE - Patch 14 |

## Patch Plan

### Patch 1 - Demux Seek Callback Lifecycle

目标：

- 消除 `seek_callback_` data race。
- 确保 initial seek 被 demux loop 消费前，decode callback 已经完成注册。

可接受做法：

- 将 demux open 与 demux thread start 拆成两个阶段：同步 open/probe/stats，wire callbacks，再 start thread。
- 或先做保守修复：给 callback 访问加锁/快照，并调整 `TrackPipelineManager::create_pipeline()` 的启动顺序，使 callback 注册早于 worker loop 消费 seek。

验证：

- `python dev.py test --native-only`
- 补 native regression：initial seek / callback registered before demux consumes seek。

### Patch 2 - Audio Pause Semantics

目标：

- paused 时 waveOut 输出 silence，但不消费任何 PCM timeline。
- playing 时仍 discard inactive tracks，保持当前多轨音频策略。

验证：

- `python dev.py test --native-only`
- 补 native test：pause 一段时间后 resume，PCM buffer 不应被 pause path 清空。

### Patch 3 - RenderSink Track Lifetime

目标：

- `RenderSink::evaluate()` 不再依赖可能悬空的长期裸 `TrackBuffer*`。
- 明确 render decision 与 track mutation 的锁/快照契约。

可接受做法：

- 每帧在 `state_mutex_` 下生成 track snapshot，再传给 sink evaluate。
- 或将 sink 内部改为 `shared_ptr`/stable handle snapshot。
- 如果暂时保留裸指针，必须让 `evaluate()` 和 `set_track/remove/compact` 共享同一把锁，并写下 assertable contract。

验证：

- `python dev.py test --native-only`
- 补 add/remove/compact while render loop active stress。
- 若影响上屏：`python dev.py ui-test --build ui_tests/smoke/basic.csv`。

### Patch 4 - Headless Texture In-Flight Tracking

目标：

- Flutter release callback 驱动 buffer availability。
- `pick_free_buffer()` 不覆盖 front 或 in-flight texture。
- 三缓冲不够时宁可 drop/reuse policy 明确，不静默覆盖。

验证：

- `python dev.py test --native-only`
- 补 delayed release test。
- 影响 Texture 上屏时：`python dev.py ui-test --build ui_tests/smoke/basic.csv`。

### Patch 5 - AnalysisManager Session Snapshot

目标：

- `loaded_ / vac2_base_ / analysis_path_` 不再作为无锁 mutable singleton state 被 render thread 读取。
- render thread 读 immutable-ish session snapshot。
- overlay chunk index 至少过滤当前 base 的 `codec / content_revision / track_index / feature_flags`。

验证：

- `python dev.py test --native-only`
- 补 load/unload/set_overlay_track/read_overlay_frame 并发 smoke。
- 影响 analysis overlay 时：`python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv`。

## Owner Boundary Backlog

这些是后续拆分方向，不抢在前五个 stabilization patch 前面。

| 边界 | 当前判断 | 建议 |
| --- | --- | --- |
| `Renderer` | 仍是最大 coordination root，但直接大拆风险高 | 按 `NATIVE_REFACTOR_TODO.md` 的顺序分轮移动状态所有权 |
| `FrameCaptureService` | 边界清楚，能顺带修 capture 锁粒度 | DONE - Patch 15 |
| `AudioEngine::Impl` | 新的强 God Object | 先修 pause 语义，再拆 `WaveOutDevice` / mixer / registry |
| `AnalysisManager` | 隐形 global/session/cache/overlay God Object | 先做 session snapshot，再谈完整 registry/session 拆分 |
| `windows/runner/video_renderer_plugin.cpp` | app bridge God Module | 后续拆 dispatcher / texture bridge / diagnostics / capture |
| `ffi_exports.cpp` | ABI God Module 苗头 | 后续拆 ABI shim / registry / commands / marshalling |

## Progress Log

每个 patch 完成后追加记录：

```text
YYYY-MM-DD Patch N - title
Changed:
Verified:
Blocked:
Follow-up:
```

2026-05-14 Patch 1 - Demux Seek Callback Lifecycle

Changed:

- Added two-stage `DemuxThread::open()` / `start_thread()` while keeping legacy `start()` as open + start.
- Protected `seek_callback_` with a mutex and used a local callback snapshot in the demux loop.
- Changed pipeline creation to open demux and start decode first; Renderer now wires the final seek callback, registers audio, and only then starts the demux worker.
- Added a demux regression test proving a pending initial seek is not consumed before callback wiring.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_initial_seek_guard.csv ui_tests/seek/playing_exact_seek_keeps_state.csv`

Blocked:

- None.

Follow-up:

- S2 audio pause semantics is next.

2026-05-14 Patch 2 - Audio Pause Semantics

Changed:

- Extracted `AudioMixer` from `WaveOutOutput` so waveOut keeps device submission ownership while mixer owns PCM consumption, silence, active track, and crossfade policy.
- Changed paused render to output silence without discarding or advancing any PCM timeline.
- Kept playing render behavior aligned with the existing policy: consume the active track and discard inactive tracks.
- Added mixer regression tests for paused silence without buffer consumption and playing consumption of the active track.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- S3 RenderSink track lifetime is next.

2026-05-14 Patch 3 - RenderSink Track Lifetime

Changed:

- Changed `TrackPipeline::track_buffer` to shared ownership so the render sink can pin buffers while evaluating a decision.
- Changed `RenderSink` to store `std::shared_ptr<TrackBuffer>` handles behind its own mutex and snapshot track handles plus offsets before evaluating.
- Removed the long-lived raw `TrackBuffer*` sink registration path from Renderer add/recreate/compact flows.
- Added a native regression proving a registered track remains valid after the original owner releases its handle.
- Added `ui_tests/track/remove_middle_compact_regression.csv` to cover remove-middle compaction and re-add order.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_last_track_readd_regression.csv ui_tests/track/remove_middle_compact_regression.csv`

Blocked:

- None.

Follow-up:

- S4 headless texture in-flight tracking is next.

2026-05-14 Patch 4 - Headless Texture In-Flight Tracking

Changed:

- Added explicit shared texture leases with buffer index and generation.
- Marked headless shared buffers in-flight when Flutter acquires a descriptor, and returned them through `FlutterDesktopGpuSurfaceDescriptor::release_callback`.
- Changed buffer reuse to skip the current front buffer and all in-flight buffers; when all buffers are busy, the frame is dropped instead of silently overwriting sampled content.
- Used an in-flight reference count so repeated acquires of the same front buffer require matching releases.
- Added generation checks so stale releases from pre-resize/pre-shutdown textures cannot unlock new buffers.
- Added a native D3D11 regression for exhausted in-flight buffers, duplicate acquires, and stale generation releases.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- S5 AnalysisManager session snapshot is next.

2026-05-14 Patch 5 - AnalysisManager Session Snapshot

Changed:

- Replaced mutable `loaded_ / vac2_base_ / analysis_path_` manager state with a shared immutable session snapshot.
- Moved overlay chunk index and overlay frame cache into the session so render reads can safely outlive concurrent load/unload swaps.
- Filtered overlay chunks against the current base session by codec, content revision, track index, and required CU geometry feature flags.
- Removed the public raw `vac2_base()` reference accessor and adjusted the only test caller.
- Added native regressions for stale overlay chunks and concurrent load/unload/read access.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/overlay_controls_h264.csv`

Blocked:

- None.

Follow-up:

- Run final cross-check against the original chat review files, then start second-tier stabilization backlog.

2026-05-14 Patch 6 - Frame Capture Lock Granularity

Changed:

- Split headless front-buffer capture into a short `snapshot_front_buffer_locked()` step and a `capture_front_buffer_snapshot()` GPU readback step.
- Kept Renderer capture serialized by `device_mutex_`, but release `texture_mutex()` before staging texture creation, `CopyResource`, `Flush`, and `Map`.
- Pinned the source texture with a ComPtr snapshot so resize/shutdown can reset current shared buffers without invalidating an in-progress capture.
- Added a native D3D11 regression proving capture reads from the pinned pre-resize snapshot.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- S7 layout validation is next.

2026-05-14 Patch 7 - Layout Validation Guardrails

Changed:

- Added native layout split/zoom constants and used them in both validation and Renderer layout application.
- Tightened `validate_layout_state()` to reject out-of-range split positions, zoom outside `[1, 50]`, invalid negative order entries, and duplicate positive file IDs.
- Kept order validation aligned with the actual project model: order entries are file IDs, with `-1`/`0` accepted as placeholders rather than slot indexes.
- Made `Renderer::apply_layout()` run the same validation so direct native callers cannot bypass FFI/runner checks and get silent clamp/fallback behavior.
- Added native layout validation regressions for split, zoom, and file-ID order semantics.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/split_screen_edges_regression.csv ui_tests/viewport/split_handle_drag_regression.csv`

Blocked:

- None.

Follow-up:

- S8 `TextureManager::create_rgba_texture()` dimension validation is next.

2026-05-14 Patch 8 - Texture Dimension Guardrails

Changed:

- Added shared texture dimension/stride helpers in `TextureManager` using `kMaxRendererDimension`.
- Rejected invalid RGBA texture dimensions before casting width/height to `UINT`.
- Reused the same guardrails for plane/NV12/P010 texture creation and upload stride checks to avoid local integer multiplication traps.
- Added a native D3D11 regression for invalid RGBA dimensions including zero, negative, and above-budget sizes.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- S9 demux read error propagation is next.

2026-05-14 Patch 9 - Demux Read Error Propagation

Changed:

- Added a `DemuxThread` read-error callback and deterministic forced-read-error test hook.
- Non-EOF demux read errors now emit the callback before aborting queues; EOF remains the normal EOF signal.
- Renderer wires demux read errors to `TrackBuffer` Error state and emits `trackError` renderer events.
- Windows runner and Dart event parsing now carry `trackError` with `errorCode`.
- Added a native regression for callback emission plus queue abort on read error.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- S10 `avcodec_open2()` SEH guard is next.

2026-05-14 Patch 10 - Codec Open SEH Guard

Changed:

- Added a noinline SEH-safe `avcodec_open2()` wrapper matching the existing send/receive guard style.
- Routed both the initial codec open and hardware-to-software fallback open through the wrapper.
- Added a per-instance codec-open test hook so native tests can raise a Windows SEH exception deterministically.
- Added a native regression proving codec-open SEH fails closed instead of escaping the test process.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/codec/av1_decode_smoke.csv`

Blocked:

- None.

Follow-up:

- S11 odd-dimension software path compatibility is next.

2026-05-15 Patch 11 - Odd-Dimension Software Frames

Changed:

- Relaxed CPU YUV layout validation to allow odd display dimensions while allocating even padded coded NV12/P010 buffers.
- Preserved original frame width/height for display and added coded width/height metadata to CPU NV12 storage.
- Updated CPU packers to duplicate edge luma/chroma samples into padded rows/columns for 4:2:0, 4:2:2, 4:4:4, 8-bit, and 10-bit software inputs.
- Allowed direct planar YUV420 frames to expose ceil-sized U/V planes instead of rejecting odd dimensions.
- Updated the D3D11 presenter to create even coded textures and scale sampling back to the original odd display size.
- Added native regressions for odd planar YUV420, odd packed YUV444-to-NV12, and padded D3D software NV12 presentation.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- S12 D3D shutdown `ClearState + Flush` cleanup is next.

2026-05-15 Patch 12 - D3D Shutdown Flush

Changed:

- `D3D11Device::shutdown()` now calls `ClearState()` and `Flush()` before releasing swap chain, context, and device references.
- Added a native regression that keeps an external immediate-context reference alive, binds an RTV, calls shutdown, and verifies the binding is cleared.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- At Patch 12 close, the remaining `review_native.md` backlog was the broader #6/#7 facade/FFI lifecycle cleanup pair.

2026-05-15 Patch 13 - NativePlayer Facade Lifecycle Guard

Changed:

- Moved `NativePlayer` public facade methods out of inline header forwards and behind the player lifecycle mutex.
- Added a single initialized/renderer-ready gate for control, query, texture, layout, capture, and dynamic track calls.
- Made pre-initialize/post-shutdown calls fail closed with stable defaults instead of touching renderer state.
- Kept frame/event callback registration serialized through the same lifecycle mutex so runner callback cleanup cannot race shutdown.
- Added a native regression covering pre-initialize and post-shutdown facade calls.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- First native-only run hit a transient `analysis_tests` scheduling failure (`read_count > 0`); isolated rerun passed, then full native-only rerun passed.

Follow-up:

- #7 FFI long-operation serialization is the remaining `review_native.md` accepted backlog item.

2026-05-15 Patch 14 - FFI Shared Player Leases

Changed:

- Replaced the FFI per-player exclusive mutex lease with a shared gate plus unique destroy/closing gate.
- Split per-player `last_error` storage behind a small error mutex so error reads/writes no longer depend on the operation gate.
- Changed `NativePlayer` lifecycle locking to shared/unique: initialize/shutdown remain exclusive, initialized facade calls share the lifecycle gate.
- Kept destroy semantics strict: unregister first, mark closing under the unique gate, then shutdown after all in-flight leases exit.
- Expanded the pure C FFI smoke with concurrent reader/writer calls while destroy races the handle.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- `review_native.md` correctness backlog is now fully fixed. Continue broader owner-boundary cleanup from `review_godobject.md`, `review_overlay.md`, and `split_adv.md`.

2026-05-15 Patch 15 - FrameCaptureService Boundary

Changed:

- Added `FrameCaptureService` as the native-facing boundary for headless front-buffer BGRA capture.
- Moved capture lock choreography out of `Renderer::capture_front_buffer()` while preserving the order `lifecycle_mutex_ -> device_mutex_ -> texture_mutex()`.
- Kept `texture_mutex()` scoped only to pinning the front-buffer snapshot; GPU copy/map remains serialized by `device_mutex_`.
- Added a D3D11 native regression covering service-level capture of the current headless front buffer.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- Continue owner-boundary cleanup one slice at a time; next candidates are `ViewportCaptureService` in the runner or a narrow `LayoutController` extraction.

2026-05-15 Patch 16 - Runner ViewportCaptureService

Changed:

- Added `ViewportCaptureService` in the Windows runner to own viewport capture orchestration, BGRA hashing/statistics, and WIC PNG persistence.
- Kept `captureViewport` MethodChannel payload and error codes unchanged.
- Reduced `video_renderer_plugin.cpp` by moving capture helper logic out of the bridge God Module.
- Marked the matching `NATIVE_REFACTOR_TODO.md` runner split item complete.

Verified:

- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- Continue runner plugin cleanup with another narrow bridge slice, or switch back to native `LayoutController` extraction.

2026-05-15 Patch 17 - LayoutController Order Boundary

Changed:

- Added `LayoutController` to own Flutter file-id order and shader slot-order translation.
- Removed `Renderer::file_id_order_`; Renderer now asks the controller to apply, snapshot, append, remove, and rebuild layout order.
- Kept viewport math and geometry updates in Renderer for now to avoid a broad layout/render split.
- Added native coverage for file-id to slot-order translation, snapshots, append, and removal.

Verified:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/split_screen_edges_regression.csv ui_tests/viewport/split_handle_drag_regression.csv`

Blocked:

- None.

Follow-up:

- If layout cleanup continues, extract viewport math separately from `display_pixel_size_for_layout_locked()` and geometry updates.

2026-05-15 Patch 18 - Runner NativeDiagnosticsProvider Process Slice

Changed:

- Added `NativeDiagnosticsProvider` in the Windows runner for process memory, process heap, and DXGI dedicated memory queries.
- Removed those helper structs/functions from `video_renderer_plugin.cpp`; the plugin now delegates diagnostics collection without changing MethodChannel or FFI payloads.
- Kept player/global diagnostics in the plugin for this slice so the process-global `g_player_weak` cleanup can remain a separate, reviewable patch.
- Updated the runner split backlog to mark the process/DXGI diagnostics slice complete and keep player/global diagnostics as follow-up.

Verified:

- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/smoke/profiler_overlay.csv`

Blocked:

- None.

Follow-up:

- Move native/player diagnostics aggregation behind the provider or a per-engine registry, then remove the process-global player weak pointer.

2026-05-15 Patch 19 - Runner NativeLoggingBootstrap

Changed:

- Added `NativeLoggingBootstrap` in the Windows runner to own default native log path selection, process-role log file naming, log-file sanitization, `vr::configure_logging()`, startup trace flush, and crash handler opt-in.
- Removed logging/crash bootstrap helpers and state from `video_renderer_plugin.cpp`; the plugin now delegates startup logging and `initLogging` reconfiguration to the bootstrap.
- Kept `initLogging` MethodChannel arguments and success/error behavior unchanged.
- Marked the matching runner split item complete in `NATIVE_REFACTOR_TODO.md`.

Verified:

- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv`

Blocked:

- None.

Follow-up:

- Keep remaining runner plugin cleanup focused on dispatcher, texture bridge, and player diagnostics/global state boundaries.

2026-05-15 Patch 20 - Runner FlutterTextureBridge

Changed:

- Added `FlutterTextureBridge` in the Windows runner to own Flutter texture registration, DXGI shared-handle surface descriptor production, release callbacks, and frame-available notifications.
- Removed texture registrar state, descriptor state, and release-context helper code from `video_renderer_plugin.cpp`.
- Kept player creation/shutdown, global player registration, event callbacks, and MethodChannel result payloads in the plugin for this slice.
- Marked the matching runner split item complete in `NATIVE_REFACTOR_TODO.md`.

Verified:

- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Follow-up:

- Continue with MethodChannel dispatcher extraction or player diagnostics/global-state isolation as separate patches.

2026-05-15 Patch 21 - Runner NativePlayerRegistry

Changed:

- Added `NativePlayerRegistry` in the Windows runner to own the process-global active player weak pointer and mutex.
- Removed exported `g_player_weak` / `g_player_mutex` state from `video_renderer_plugin.h/.cpp`; plugin code now publishes, clears, and pins through registry helpers.
- Kept the current process-global diagnostics semantics unchanged for this slice, but made the remaining global-state boundary explicit.
- Updated `NATIVE_REFACTOR_TODO.md` to track the registry wrapper as done and keep plugin/provider-scoped diagnostics as follow-up.

Verified:

- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/smoke/profiler_overlay.csv ui_tests/analysis/spawn_h264.csv`

Blocked:

- None.

Follow-up:

- Move diagnostics active-player lookup from process-global registry to plugin/provider scope, then delete the global registry if secondary-engine stats no longer need it.

2026-05-15 Patch 22 - NativeDiagnosticsProvider MethodChannel Payload

Changed:

- Moved MethodChannel `getDiagnostics` payload assembly into `NativeDiagnosticsProvider`.
- Moved GPU memory breakdown map assembly out of `video_renderer_plugin.cpp`.
- Kept the active-player lease source and all diagnostics field names unchanged for this slice.
- Split the diagnostics backlog into completed MethodChannel aggregation and remaining FFI/provider-scope work.

Verified:

- `python dev.py ui-test --build ui_tests/smoke/profiler_overlay.csv`

Blocked:

- None.

Follow-up:

- Move the FFI flat diagnostics export through the provider, then tackle active-player lookup scope separately.

2026-05-15 Patch 23 - NativeDiagnosticsProvider FFI Payload

Changed:

- Added `native_diagnostics_ffi.h` for the stable Dart FFI diagnostics structs.
- Moved FFI flat diagnostics filling into `NativeDiagnosticsProvider::FillFfiDiagnostics()`.
- Reduced `naki_vr_get_diagnostics()` to the exported ABI shim plus active-player lookup.
- Kept the exported symbol, struct layout, and Dart FFI contract unchanged.

Verified:

- `python dev.py ui-test --build ui_tests/smoke/profiler_overlay.csv`

Blocked:

- None.

Follow-up:

- Move diagnostics active-player lookup from the process-global registry to plugin/provider scope.

2026-05-15 Patch 24 - Runner FilePickerService

Changed:

- Added `FilePickerService` in the Windows runner to own the file-open dialog, video filters, multi-select option, and UTF-16 path conversion.
- Reduced `VideoRendererPlugin::PickFiles()` to argument parsing and MethodChannel list marshalling.
- Kept `pickFiles` payload behavior unchanged: cancel, dialog failure, or no usable file path returns an empty list.
- Marked the file picker runner split item complete in `NATIVE_REFACTOR_TODO.md`.

Verified:

- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Blocked:

- None.

Coverage gap:

- No non-interactive UI automation currently selects real files through the native Windows file dialog.

Follow-up:

- Continue runner plugin cleanup with dispatcher extraction or active diagnostics scope.

2026-05-15 Patch 25 - Repeated Create-Destroy UI Smoke

Changed:

- Added `ui_tests/track/repeated_create_destroy_smoke.csv`.
- Covers repeated add-media, remove-last-track, player teardown, player recreation, and final viewport capture.
- Marks the repeated create-destroy portion of the global-state test backlog complete while leaving multi-player/plugin teardown as follow-up.

Verified:

- `git diff --check`
- `python dev.py ui-test ui_tests/track/repeated_create_destroy_smoke.csv`

Blocked:

- None.

Follow-up:

- Add multi-player or plugin teardown coverage once the app exposes a non-flaky automation entrypoint for that scenario.

2026-05-15 Patch 26 - Native Player Method Dispatcher

Changed:

- Added `windows/runner/native_player_method_dispatcher.{h,cpp}` and wired it into the runner build.
- Moved MethodChannel method-name lookup out of `VideoRendererPlugin::HandleMethodCall()` into `NativePlayerMethodDispatcher`.
- Split the formerly inline MethodChannel branches into typed `VideoRendererPlugin` handlers while keeping method names, error codes, and return payloads unchanged.
- Marked the runner dispatcher split item complete in `NATIVE_REFACTOR_TODO.md`.

Verified:

- `git diff --check`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/smoke/profiler_overlay.csv ui_tests/seek/playing_exact_seek_keeps_state.csv ui_tests/viewport/viewport_pan_layout_regression.csv`

Blocked:

- None.

Follow-up:

- Continue shrinking `VideoRendererPlugin` by moving player lifecycle/control handlers behind a narrower player bridge and by removing process-global diagnostics lookup where possible.

2026-05-15 Patch 27 - Plugin-Scoped MethodChannel Diagnostics

Changed:

- Changed MethodChannel `getDiagnostics` to pass the plugin instance `player_` directly into `NativeDiagnosticsProvider`.
- Removed the MethodChannel dependency on `NativePlayerRegistry::Pin()` while preserving the diagnostics payload shape.
- Split the diagnostics backlog into completed MethodChannel instance scope and remaining FFI host/session scope.

Verified:

- `git diff --check`
- `python dev.py ui-test --build ui_tests/smoke/profiler_overlay.csv`

Blocked:

- None.

Follow-up:

- FFI `naki_vr_get_diagnostics()` still uses the process-global registry because the stats window polls it through `DynamicLibrary.executable()`; replace that with a host/session-scoped contract in a separate ABI slice.

2026-05-15 Patch 28 - FFI Logging/Crash Global Ownership Notes

Changed:

- Added public ABI comments to `native/video_renderer/exports/ffi_exports.h` documenting that logging and crash-handler FFI convenience APIs mutate process-wide state.
- Split the global-state backlog into completed API warning and remaining host-provided logger/sink design.

Verified:

- `git diff --check`
- `python dev.py test --native-only`

Blocked:

- None.

Follow-up:

- Design a host-provided logger/sink API so embedded or multi-engine hosts do not need to share global spdlog/crash-handler ownership.

2026-05-15 Patch 29 - Shutdown During Seek Recreate Smoke

Changed:

- Added `ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`.
- Covers play -> real timeline seek -> immediate last-track removal/player shutdown -> player recreation -> viewport capture.
- Split the renderer shutdown stress backlog into completed seek/recreate UI smoke and remaining capture/resize stress coverage.

Verified:

- `git diff --check`
- `python dev.py ui-test ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Blocked:

- None.

Follow-up:

- Add dedicated shutdown-during-capture and shutdown-during-resize stress coverage.

## Final Cross-Check

完成本轮后，逐条回看 chat 文件，更新下列结果：

| 来源 | 复核项 | 结果 |
| --- | --- | --- |
| `review_native.md` | 13 条 native correctness / lifecycle / validation 问题 | fixed: #1/#2/#3/#4/#5/#6/#7/#8/#9/#10/#11/#12/#13 |
| `review_godobject.md` | God Object 排名和 owner boundary 判断 | fixed: AudioMixer boundary + Analysis session snapshot; accepted-backlog: remaining owner splits |
| `review_overlay.md` | AnalysisManager、VACHUNK、overlay cache、D3D pass 风险 | fixed: AnalysisManager session + current-base chunk filter; accepted-backlog: remaining overlay/cache/render-pass items |
| `split_adv.md` | Patch 顺序和“不贪大”边界 | fixed: Patch 1-14 executed in stabilization-sized slices |

复核时只标三类状态：

- `fixed`: 本轮已修且有验证。
- `accepted-backlog`: 仍存在，但明确进入后续 backlog。
- `not-applicable`: 源码已变化或 chat 判断不再成立，并写明证据。

### `review_native.md`

fixed:

- #1 Demux seek callback race: Patch 1 split demux open/start and locked callback access.
- #2 RenderSink raw `TrackBuffer*`: Patch 3 switched sink registration to shared buffer snapshots.
- #3 Headless texture overwrite risk: Patch 4 added release-driven in-flight tracking.
- #4 capture lock/GPU wait split: Patch 6 split front-buffer snapshot from GPU readback.
- #5 Audio pause discards PCM: Patch 2 made paused render output silence without consuming PCM.
- #6 NativePlayer facade locking: Patch 13 moved public methods behind the lifecycle mutex and fail-closed initialized gate.
- #7 FFI long-operation serialization: Patch 14 uses shared player leases, unique destroy gating, and a separate error mutex.
- #8 layout validation: Patch 7 tightened split/zoom/order checks using file-ID order semantics.
- #9 RGBA texture size validation: Patch 8 added texture dimension/stride guardrails.
- #10 demux read error propagation: Patch 9 emits track error events and marks the track buffer Error on non-EOF read errors.
- #11 `avcodec_open2()` SEH guard: Patch 10 routes codec open and software fallback open through a noinline SEH wrapper.
- #12 odd-dimension software path: Patch 11 pads coded CPU NV12/P010 buffers while preserving odd display dimensions.
- #13 D3D shutdown `ClearState + Flush`: Patch 12 clears immediate-context bindings and flushes before device release.

not-applicable:

- None.

### `review_godobject.md`

fixed:

- `AudioEngine::Impl`: Patch 2 extracted `AudioMixer`, separating output submission from mixer/PCM consumption policy.
- `AnalysisManager`: Patch 5 introduced session snapshots and moved overlay chunk cache/index state into the session.
- Windows runner plugin diagnostics: Patch 18 moved process/heap/DXGI memory queries into `NativeDiagnosticsProvider`.
- Windows runner logging/crash bootstrap: Patch 19 moved app-layer logging and crash handler opt-in into `NativeLoggingBootstrap`.
- Windows runner texture bridge: Patch 20 moved texture registration, shared-handle descriptor fill, release callbacks, and frame notifications into `FlutterTextureBridge`.
- Windows runner global player state: Patch 21 wrapped the process-global active player weak pointer in `NativePlayerRegistry`.
- Windows runner MethodChannel diagnostics: Patch 22 moved native/player diagnostics payload assembly into `NativeDiagnosticsProvider`.
- Windows runner FFI diagnostics: Patch 23 moved flat struct filling into `NativeDiagnosticsProvider` behind a stable ABI header.
- Windows runner file picker: Patch 24 moved native file dialog and path conversion into `FilePickerService`.
- Global-state smoke coverage: Patch 25 added repeated create-destroy UI coverage.
- Windows runner MethodChannel dispatch: Patch 26 moved method-name lookup into `NativePlayerMethodDispatcher`.
- Windows runner MethodChannel diagnostics scope: Patch 27 switched `getDiagnostics` to plugin instance player scope.
- FFI logging/crash ownership notes: Patch 28 documented process-global ownership in the public FFI header.
- Renderer shutdown/seek smoke coverage: Patch 29 added shutdown during real seek plus recreate UI coverage.

accepted-backlog:

- `Renderer` remains the root coordination object; future work should move one owner boundary at a time.
- `windows/runner/video_renderer_plugin.cpp` remains a bridge God Module; split player handlers and FFI diagnostics host/session scope later.
- `ffi_exports.cpp` remains an ABI God Module candidate; split ABI shim / registry / commands / marshalling later.
- `TrackPipelineManager` remains a lifecycle-heavy factory; further factory/lifecycle split remains useful.
- `DecodeThread`, `FrameConverter`, target boundaries, process globals, and resource-budget policy remain second-stage refactor topics.

not-applicable:

- None.

### `review_overlay.md`

fixed:

- AnalysisManager lifecycle data race: Patch 5 replaced mutable singleton session fields with shared session snapshots.
- Overlay chunk consistency: Patch 5 filters overlay chunks by codec, base content revision, track index, and required CU geometry feature flags.

accepted-backlog:

- VACHUNK hot-path IO/cache amplification remains a cache/LRU or chunk-layout follow-up.
- VACache atomic replace and tmp-name uniqueness remain a cache publication follow-up.
- `overlay_raster.cpp` helper UB/bounds checks remain a raster hardening follow-up.
- D3D overlay pass state contract remains a render-pass cleanup.
- 16-bit rect precision tests, generation budget details, checksum validation, record-count guards, opacity 0 semantics, and deeper VAC2 frame modeling remain overlay backlog.

not-applicable:

- None.

### `split_adv.md`

fixed:

- Patch 1-5 were completed in the recommended stabilization shape: one owner/lifetime/threading issue per patch, with native tests and relevant UI tests.
- Documentation/cross-check completed before starting the second-tier backlog.
- Patch 6 completed the S6 capture lock-granularity cleanup without a large Renderer split.
- Patch 7 completed the S7 layout validation guardrails as a narrow defensive patch.
- Patch 8 completed the S8 texture dimension guardrail as a small low-risk patch.
- Patch 9 completed the S9 demux read-error propagation as a narrow error-model patch.
- Patch 10 completed the S10 codec-open SEH guard as a narrow decode hardening patch.
- Patch 11 completed the S11 odd-dimension software-frame compatibility patch without adding generic scaling fallback.
- Patch 12 completed the S12 D3D shutdown cleanup as a narrow lifecycle patch.
- Patch 13 completed the `NativePlayer` facade lifecycle guard as a narrow boundary patch.
- Patch 14 completed the FFI shared player lease cleanup as a narrow ABI/registry patch.
- Patch 15 completed the native-facing `FrameCaptureService` boundary without touching runner PNG/WIC capture.
- Patch 16 completed the runner-facing `ViewportCaptureService` slice while preserving MethodChannel behavior.
- Patch 17 completed the first `LayoutController` slice for order ownership, intentionally leaving viewport math in Renderer.
- Patch 18 completed the first runner `NativeDiagnosticsProvider` slice for process/heap/DXGI memory queries.
- Patch 19 completed the runner `NativeLoggingBootstrap` slice for startup logging and crash handler opt-in.
- Patch 20 completed the runner `FlutterTextureBridge` slice for Texture registrar and shared-handle callback ownership.
- Patch 21 completed the first global-player-state slice by wrapping the active player weak pointer in `NativePlayerRegistry`.
- Patch 22 completed MethodChannel diagnostics aggregation inside `NativeDiagnosticsProvider`.
- Patch 23 completed FFI diagnostics aggregation inside `NativeDiagnosticsProvider`.
- Patch 24 completed the runner `FilePickerService` slice for native file dialog ownership.
- Patch 25 added repeated create-destroy UI smoke coverage for last-track teardown and player recreation.
- Patch 26 completed the runner `NativePlayerMethodDispatcher` slice for MethodChannel method-name dispatch.
- Patch 27 completed MethodChannel diagnostics active-player lookup scope.
- Patch 28 documented process-global logging/crash FFI ownership and split out host-provided logger/sink design.
- Patch 29 added a shutdown-during-seek recreate smoke test.

accepted-backlog:

- Remaining second-priority owner boundary work should continue as explicit owner-boundary slices; avoid jumping straight into a large Renderer split.

not-applicable:

- None.

## Archived Patch Queue P30-P99

### P30 - VACache Atomic Publish

Status: done in Patch 30.

Source: `review_overlay.md`.

Goal:

- Replace final cache files atomically.
- Stop deleting the final path before rename.
- Make tmp file names unique across PID/TID/counter/key to avoid same-process or multi-process collision.

Likely files:

- `native/analysis/cache/vacache_store.*`
- related tests under `native/tests/analysis/`

Validation:

- `git diff --check`
- `python dev.py test --native-only`

### P31 - VACHUNK Hot-Path Cache

Status: done in Patch 31.

Source: `review_overlay.md`.

Goal:

- Avoid re-opening and decoding full `FSUM/FIDX/CU4R` sections for every adjacent frame.
- Add a small decoded chunk cache/LRU keyed by chunk path and metadata.
- Keep behavior unchanged for corruption or stale-cache rejection.

Likely files:

- `native/analysis/manager/analysis_manager.*`
- `native/analysis/parsers/vachunk_parser.*`
- analysis tests.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/overlay_controls_h264.csv`

### P32 - Overlay Raster Hardening

Status: done in Patch 32.

Source: `review_overlay.md`.

Goal:

- Remove strict-aliasing / object-lifetime risk from BGRA fill helpers.
- Guard zero or negative dimensions.
- Guard surface byte-size multiplication overflow.
- Add focused native tests.

Likely files:

- `native/analysis/cache/overlay_raster.*`
- `native/tests/analysis/`

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/overlay_controls_h264.csv`

### P33 - D3D Overlay Pass Contract

Status: done in Patch 33.

Source: `review_overlay.md`, `review_godobject.md`.

Goal:

- Make analysis overlay D3D pass state ownership explicit.
- Ensure every pass either restores touched state or the caller fully rebinds the next pass state.
- Unbind SRV/RTV hazards deliberately.

Likely files:

- `native/video_renderer/renderer.cpp`
- D3D overlay helper/resource files.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/overlay_controls_h264.csv`

### P34 - Overlay Precision And Semantics Tests

Status: done in Patch 34.

Source: `review_overlay.md`.

Goal:

- Add regression coverage for tiny rects, edge rects, shared boundaries, 4K/8K-style coordinate ranges, grid-only paths, and opacity 0.
- Fix behavior only where tests expose incorrect semantics.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/overlay_controls_h264.csv`

### P35 - Overlay Generation Budget And Enum Semantics

Status: done in Patch 35.

Source: `review_overlay.md`.

Goal:

- Make overlay chunk generation honor the current-hash remaining budget instead of the raw global cache byte limit.
- Replace or pin the codec enum conversion used for chunk keys so chunk path/read matching cannot silently diverge.

Likely files:

- `windows/runner/analysis_ffi.cpp`
- `native/analysis/cache/vacache_store.*`
- related native analysis tests.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/overlay_controls_h264.csv`

### P36 - VACHUNK Checksum Semantics

Status: done in Patch 36.

Source: `review_overlay.md`.

Goal:

- Decide and implement checksum behavior for VACHUNK header/section checksum fields.
- If checksum remains unused, make the reserved/zero semantics explicit and validated; if enabled, verify corruption detection on read.

Likely files:

- `native/analysis/parsers/vachunk_parser.*`
- `native/tests/analysis/test_analysis_parsers.cpp`
- `native/docs/formats/VACHUNK.md`

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/overlay_controls_h264.csv`

### P37 - VACHUNK Record Count Guard

Status: done in Patch 37.

Source: `review_overlay.md`.

Goal:

- Guard `make_vachunk_record_section()` record-count narrowing before writing.
- Keep caller-visible behavior deterministic instead of letting malformed sections fail later in generic write validation.

Likely files:

- `native/analysis/parsers/vachunk_parser.*`
- `native/tests/analysis/test_analysis_parsers.cpp`

Validation:

- `python dev.py test --native-only`

### P38 - VAC2 Frame Model Assumptions

Status: done in Patch 38.

Source: `review_overlay.md`.

Goal:

- Make the current packet-index-to-frame fallback explicit in metadata/docs/tests so future overlay alignment work has a hard boundary.
- Add regression coverage around frame/packet mapping assumptions that overlay lookup depends on.

Likely files:

- `native/analysis/generators/analysis_generator.*`
- `native/analysis/parsers/vac2_parser.*`
- `native/tests/analysis/test_analysis_generator.cpp`
- `native/docs/formats/VAC2.md`

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/overlay_controls_h264.csv`

## Godobject Patch Queue

### P39 - Renderer Analysis Overlay Boundary

Status: done in Patch 39.

Source: `review_godobject.md`, now that `review_overlay.md` is fixed.

Goal:

- Move analysis overlay drawing/resource helpers behind a small renderer-adjacent owner instead of keeping all overlay pass logic as `Renderer` methods.
- Keep D3D state contract and overlay behavior unchanged.
- Do not touch playback, seek, track lifecycle, or shader semantics in this patch.

Likely files:

- `native/video_renderer/renderer.*`
- new `native/video_renderer/analysis_overlay_renderer.*` or equivalent.
- `native/video_renderer/CMakeLists.txt` / source list if needed.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/overlay_controls_h264.csv`

Result:

- Added `AnalysisOverlayRenderer` as the renderer-adjacent owner for overlay CPU cache, packed rect upload buffers, mask materialization, and final overlay draw calls.
- Kept `D3D11RenderResources` as the D3D resource storage for this patch so ownership and render-thread timing stay unchanged.
- Preserved the existing `pack_overlay_uv16` test contract while moving implementation out of `renderer.cpp`.

### P40 - Renderer Layout Ownership Continuation

Status: done in Patch 40.

Source: `review_godobject.md`, `native/docs/NATIVE_REFACTOR_TODO.md`.

Goal:

- Continue moving layout/order calculations out of `Renderer` without changing public layout behavior.
- Target pure decision/state helpers first; avoid mixing this with render-loop or track lifecycle changes.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/viewport_pan_layout_regression.csv`

Result:

- Moved layout protocol constants and `LayoutState` into `layout_state.h` so validation/controller code no longer reaches through `renderer.h`.
- Added `layout_geometry` as a pure layout math owner for display pixel-size, track scale, display offset, inverse display size, and view-offset UV constants.
- `Renderer` now snapshots layout/track geometry, fills non-layout color/format constants, and leaves layout shader math outside the god object.
- Added native coverage for uniform pixel-size shader constants and resize display-size math.

### P41 - Render Loop Boundary

Status: done in Patch 41.

Goal:

- Extract render-loop timing/device-lost decision boundaries only after overlay/layout state is calmer.
- Keep immediate D3D context ownership explicit.

Validation:

- `python dev.py test --native-only`
- smoke + timeline/viewport UI scripts depending on touched paths.

Result:

- Added `RenderLoopController` as the owner for resize debounce timing, periodic diagnostics cadence, and frame deadline sleep calculation.
- Removed the resize debounce timestamp and diagnostic timestamp/PTS bookkeeping from `Renderer::render_loop()`.
- Kept D3D immediate-context ownership and present/publish paths inside `Renderer` for this patch.
- Added native coverage for resize debounce, diagnostic delta calculation, and max-capped frame sleep.

### P42 - FFI ABI God Module Split

Status: done in Patch 42.

Goal:

- Move the player handle registry, closing gate lease, thread-local last error, and per-player error state out of `ffi_exports.cpp`.
- Keep extern "C" ABI entrypoints and command marshalling unchanged in this first FFI split.

Validation:

- `python dev.py test --native-only`
- FFI C validation already included in native tests.

Result:

- Added `ffi_player_registry` as the owner for live player map registration, unregister/pin, gate-locked `PlayerLease`, and invalid/destroyed handle reporting.
- `ffi_exports.cpp` now includes the registry boundary and keeps ABI guards, struct marshalling, and exported command functions for follow-up splits.
- `video_renderer_ffi` now builds the registry translation unit explicitly.

### P43 - TrackPipelineManager Lifecycle Split

Status: done in Patch 43.

Goal:

- Split slot storage, pipeline factory, and start/stop/recreate lifecycle order.
- Make callback wiring before thread start an explicit invariant.

Validation:

- `python dev.py test --native-only`
- relevant track/seek UI script if behavior-facing paths move.

Result:

- Added `TrackPipelineFactory::create_opened_pipeline()` for queue/controller construction, synchronous demux open, track-buffer sizing, decode-thread creation, and hardware decode mode selection.
- Removed pipeline construction from `TrackPipelineManager`, which now stays on slot lookup, stop, clear, and compact responsibilities.
- Kept the demux worker start in `Renderer` after seek/error/audio callbacks are wired; added native coverage that wires a seek callback before `start_thread()` and observes the pending seek callback.
- Verified runner-facing track lifecycle paths with smoke, middle-track compact/re-add, and shutdown-during-seek recreate UI scripts.

### P44 - DecodeThread State-Machine Guards

Status: done in Patch 44.

Goal:

- Add focused tests and helper objects for seek/drain/exact-preview state combinations before large extraction.

Validation:

- `python dev.py test --native-only`

Result:

- Added `exact_seek_window` as a pure helper for exact-seek lookbehind collection, preview-window readiness, and selected preview index fallback.
- Replaced the duplicated exact-seek selection checks inside `DecodeThread` with calls into the helper while keeping frame conversion, hardware waits, and publish order inside the decode thread.
- Added native unit coverage for pre-target lookbehind bounds, post-target window readiness, first-frame exact-target selection, latest pre-target fallback, and EOF/all-pre-target fallback.
- Verified runner-facing exact seek behavior with smoke and HEVC visual seek UI scripts.

### P45 - Native Budget Policy Consolidation

Status: done in Patch 45.

Goal:

- Centralize queued-frame, exact-seek, analysis-cache, capture, and runtime memory budget rules into explicit policy objects.

Validation:

- `python dev.py test --native-only`
- UI scripts selected by touched policy surface.

Result:

- Added `TrackBufferBudget` as the explicit policy boundary for high-resolution detection and TrackBuffer forward/backward frame depth.
- `TrackPipelineFactory` now consumes a budget decision instead of embedding the high-resolution queued-frame rule.
- Added native coverage for invalid/small tracks, high-resolution software tracks, and reduced-depth high-resolution hardware tracks.
- Verified runner-facing add/seek/render paths with smoke and AV1 codec visual UI scripts.

### P46 - DecodeThread Drain/Flush Guards

Status: done in Patch 46.

Goal:

- Continue P44 by isolating drain-before-next-packet, EOF flush, post-seek pause, and cancellation guard decisions into small tested helpers.
- Keep codec send/receive, AVFrame ownership, and hardware visibility flush calls inside `DecodeThread` until the guard semantics are pinned.

Validation:

- `python dev.py test --native-only`
- HEVC seek/step UI script if decode seek behavior changes.

Result:

- Added `decode_loop_policy` for pending exact-seek frame publish, drain-before-next-packet, paused packet preservation, and stale packet discard guards.
- `DecodeThread` still owns FFmpeg send/receive, EOF drain, frame ownership, and hardware visibility flush calls; guard extraction did not change codec ownership.
- Added native tests for paused, flushing, seek-pending, and pending exact-seek frame combinations.
- Verified runner-facing seek/render behavior with smoke and HEVC seek visual UI scripts.

### P47 - FFI Command/Marshalling Split

Status: done in Patch 47 for the marshalling half; command-body split continues as P48.

Goal:

- Continue P42 by moving UTF-8/path marshalling and typed command bodies out of `ffi_exports.cpp`.
- Keep extern "C" exported functions as ABI shells that validate, pin the player, and forward.

Validation:

- `python dev.py test --native-only`
- FFI C validation already included in native tests.

Result:

- Added `ffi_marshalling` for ABI size/version checks, log config conversion, v1/v2 player config conversion, layout state conversion, and seek enum conversion.
- Reduced `ffi_exports.cpp` by moving struct/path marshalling out while preserving exported function names and status/last-error behavior.
- Added focused native tests for counted paths, log config, layout roundtrip, invalid counted-path storage, and seek enum conversion.
- Verified with native-only tests, C FFI validation, and a rebuilt Flutter smoke UI script.

### P48 - FFI Player Command Body Split

Status: done in Patch 48.

Goal:

- Move repetitive checked-player command bodies for playback, query, track, and layout operations behind typed helper functions.
- Keep extern "C" functions as ABI shells and avoid changing legacy wrapper return semantics.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `ffi_player_commands` for initialize/shutdown, playback, query, track, and layout command bodies while preserving checked-player lease and last-error semantics.
- `ffi_exports.cpp` is now about 397 lines and mostly retains exported ABI guards, lifecycle registry shell code, and process-global logging/crash APIs.
- Added focused native command tests for handle leases, pre-handle validation, output-slot clearing, layout marshalling reuse, and invalid-handle fallbacks.
- Verified with native-only tests, C FFI validation, and a rebuilt Flutter smoke UI script.

### P49 - DecodeThread EOF Drain/Codec Flush Policy

Status: done in Patch 49.

Goal:

- Continue P46 by isolating EOF drain and codec-flush decisions that are still embedded in the decode loop.
- Keep AVFrame ownership, codec send/receive, and hardware visibility waits inside `DecodeThread` unless tests expose a smaller safe boundary.

Validation:

- `python dev.py test --native-only`
- HEVC seek/step or smoke UI script if decode state transitions change.

Result:

- Extended `decode_loop_policy` with EOF drain action selection for Buffering exact seek, Buffering non-exact, and normal codec-drain states.
- Added exact-seek reorder publish policy for preview-window readiness and queue-EOF drain/publish behavior.
- Kept FFmpeg send/receive, AVFrame ownership, and hardware visibility flush calls inside `DecodeThread`.
- Verified with native-only tests plus rebuilt smoke and HEVC seek visual UI scripts.

### P50 - Renderer Track Lifecycle Boundary

Status: done in Patch 50.

Goal:

- Move track add/recreate/rollback sequencing out of `Renderer` into a narrow lifecycle helper without changing slot ownership or render-thread contracts.
- Keep `Renderer` as the coordination root for this patch; only remove duplicated start/rollback mechanics that already depend on `TrackPipelineFactory`.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Result:

- Added `track_lifecycle` to configure track file_id/offset/recreate flags, wire seek/error/audio hooks, start the demux worker, and rollback decode/demux/audio state on failed start.
- Updated initial load, add-track, and HEVC pipeline recreate paths to use the shared lifecycle helper.
- Added native coverage with a real opened pipeline to verify hook wiring, demux start, seek callback delivery, and stop-time unregister behavior.
- Verified with native-only tests plus rebuilt smoke, track compact, and shutdown-during-seek recreate UI scripts.

### P51 - Renderer Track Removal/Compaction Boundary

Status: done in Patch 51.

Goal:

- Move remove-track stop/compact side effects into a helper that owns render-sink/presenter slot updates and `last_decision_` compaction.
- Keep layout mutation and playback pause/resume decisions in `Renderer` for this patch.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv`

Result:

- Added `TrackRemovalHooks`, `remove_and_compact_track_pipeline`, and `compact_present_decision_frames`.
- `Renderer::remove_track` now keeps playback/layout decisions but delegates track stop/compact plus render-sink/presenter slot side effects.
- Added native coverage for slot compaction and cached present-frame compaction.
- Verified with native-only tests plus rebuilt smoke and track compact UI scripts.

### P52 - Renderer Add-Track Seek Policy

Status: done in Patch 52.

Goal:

- Move the current-clock add-track seek preparation into a small helper or policy boundary.
- Keep `Renderer` responsible for slot commit, layout mutation, and playback pause/resume.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv`

Result:

- Added `track_pts_end_us_from_stats`, `clamp_track_seek_target_us`, and `prepare_add_track_seek_to_clock` to `track_lifecycle`.
- `Renderer::add_track` now keeps slot commit/layout decisions but delegates current-clock seek preparation and request submission.
- Added native coverage for track PTS end heuristics, idle add-track seek, exact paused add-track seek, and keyframe playing add-track seek.
- Verified with native-only tests plus rebuilt smoke and track compact UI scripts.

### P53 - Renderer HEVC Seek Recreate Policy

Status: done in Patch 53.

Goal:

- Move the HEVC hardware seek recreate decision out of `Renderer::seek_internal` into a small policy boundary.
- Keep actual pipeline recreation and frame-presenter reset owned by `Renderer` for this patch.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Result:

- Added `HevcSeekRecreateInput`, `HevcSeekRecreateDecision`, and `choose_hevc_seek_recreate` to `SeekCoordinator`.
- `Renderer::seek_internal` now delegates HEVC hardware recreate/coalesce/error decisions while still executing actual pipeline recreation.
- Added native coverage for non-HEVC, playing seek recreate/error, transition coalescing, paused one-shot keyframe recreate, paused exact seek, and forced paused transition recreate cases.
- Verified with native-only tests plus rebuilt smoke and shutdown-during-seek recreate UI scripts.

### P54 - Renderer Seek Track Preparation Boundary

Status: done in Patch 54.

Goal:

- Move generic per-track seek preparation side effects out of `Renderer::seek_internal`: decode/audio pause, buffer state transition, frame clear, presenter reset trigger, packet/audio queue flush, and seek request submission.
- Keep global seek timing, playback clock update, deferred paused HEVC gate, and actual pipeline recreation owned by `Renderer` for this patch.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/timeline/h265_timeline_seek_crash.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Result:

- Added `prepare_track_seek_transition` and `submit_track_seek_after_recreate` to `track_lifecycle`.
- `Renderer::seek_internal` now delegates per-track pause/flush/reset preparation and post-recreate seek request submission.
- Added native coverage for generic track seek preparation, transition detection, packet/audio queue flushing, presenter reset hook dispatch, and recreated seek suppression.
- Verified with native-only tests plus rebuilt smoke, H.265 timeline seek crash, and shutdown-during-seek recreate UI scripts.

### P55 - Renderer Seek Pipeline Recreate Boundary

Status: done in Patch 55.

Goal:

- Move `recreate_pipeline_for_seek` stop/recreate/start/render-sink commit choreography into a lifecycle helper.
- Keep the HEVC recreate decision, pipeline factory callback, and Renderer-owned audio/render-sink hooks at the Renderer boundary.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Result:

- Added `TrackPipelineRecreateHooks` and `recreate_track_pipeline_for_seek` to `track_lifecycle`.
- `Renderer::recreate_pipeline_for_seek` now provides hooks while lifecycle code owns old-track metadata capture, stop, driver settle, replacement startup, and slot commit.
- Removed the unused decode-thread-only recreate path.
- Added native coverage using a real replacement pipeline to verify hook order, metadata preservation, recreated flag, initial seek, and commit.
- Verified with native-only tests plus rebuilt smoke and shutdown-during-seek recreate UI scripts.

### P56 - Renderer Track Add Commit Boundary

Status: done in Patch 56.

Goal:

- Move add-track render-sink/frame-presenter/tracks slot commit into a lifecycle helper.
- Keep layout mutation, duration cache update, playback pause/resume, and public file-id allocation in `Renderer`.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv`

Result:

- Added `TrackAddCommitHooks` and `commit_new_track_pipeline` to `track_lifecycle`.
- `Renderer::add_track` now delegates render-sink registration, frame-presenter reset, and `tracks_` slot installation.
- Added native coverage for add-track commit hook order, slot installation, metadata preservation, and invalid/null commit rejection.
- Verified with native-only tests plus rebuilt smoke and track compact UI scripts.

### P57 - Renderer Track Duration Cache Boundary

Status: done in Patch 57.

Goal:

- Move track duration cache max/recompute helpers out of `Renderer`.
- Keep the cached value field and public `duration_us()` API in `Renderer`.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv`

Result:

- Added `track_duration_us`, `extend_track_duration_cache`, and `compute_track_duration_cache` to `track_lifecycle`.
- `Renderer` now delegates initial duration cache build, add-track extension, and remove-track recompute.
- Added native coverage for no-demux tracks, manager recompute, and real opened pipeline duration.
- Verified with native-only tests plus rebuilt smoke and track compact UI scripts.

### P58 - Renderer Track Playback Pause Guard

Status: done in Patch 58.

Goal:

- Extract add/remove track temporary playback pause/resume decisions into a small helper or guard policy.
- Keep `Renderer` responsible for public playback state, layout mutation, and operation failure rollback.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv`

Result:

- Added `TrackPlaybackMutationHooks` and playback mutation helpers to `track_lifecycle`.
- `Renderer::add_track` now delegates temporary pause and failure rollback while preserving its success behavior.
- `Renderer::remove_track` now delegates temporary pause and conditional resume after removal.
- Added native coverage for idle, rollback, no-track removal, and active-track removal playback decisions.
- Verified with native-only tests plus rebuilt smoke and track compact UI scripts.

### P59 - Renderer Seek Target Clamp Boundary

Status: done in Patch 59.

Goal:

- Move seek target clamping and pending seek event retarget decision out of `Renderer::seek_internal`.
- Keep `Renderer` responsible for actually updating the playback clock and running deferred seek gates.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/pts_offset_seek_clamp_generated.csv`

Result:

- Added `resolve_seek_target` and pending preview event state/result structs to `SeekCoordinator`.
- `Renderer::seek_internal` now delegates requested-target clamp and pending event retarget decision before updating the playback clock.
- Added native coverage for in-range, negative, tail, unbounded, and pending event retarget cases.
- Verified with native-only tests plus rebuilt smoke and PTS-offset seek clamp UI scripts.

### P60 - Renderer Track Seek Target Boundary

Status: done in Patch 60.

Goal:

- Move per-track seek requested-target/offset clamp facts out of `Renderer::seek_internal`.
- Keep `Renderer` responsible for track iteration, logging, transition preparation, and seek request submission.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/pts_offset_seek_clamp_generated.csv`

Result:

- Added `TrackSeekTargetResolution` and `resolve_track_seek_target` to `track_lifecycle`.
- `Renderer::seek_internal` now delegates per-track offset/clamp calculation and only logs/applies the returned facts.
- Removed the now-redundant `Renderer::clamp_track_seek_target_us_locked` forwarding method.
- Added native coverage for offset subtraction, pre-offset clamp, and real media tail clamp.
- Verified with native-only tests plus rebuilt smoke and PTS-offset seek clamp UI scripts.

### P61 - Renderer Track Offset Mutation Boundary

Status: done in Patch 61.

Goal:

- Move `set_track_offset` track mutation plus render-sink offset update behind a lifecycle helper.
- Keep `Renderer` responsible for file-id lookup, lock ownership, and preview redraw invalidation.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/h265_track_offset_refresh_visual_regression.csv`

Result:

- Added `TrackOffsetMutationHooks`, `TrackOffsetMutationResult`, and `apply_track_offset_mutation` to `track_lifecycle`.
- `Renderer::set_track_offset` now delegates track state mutation and render-sink offset synchronization.
- Added native coverage for changed and unchanged offset mutation hook behavior.
- Verified with native-only tests plus rebuilt smoke and H.265 track-offset visual regression scripts.

### P62 - Renderer Track Manager Query Boundary

Status: done in Patch 62.

Goal:

- Move remaining simple active-track/count queries from `Renderer` into `TrackPipelineManager`.
- Keep `Renderer` responsible for public locking and public API shape.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv`

Result:

- Added `TrackPipelineManager::count()` and `TrackPipelineManager::first_active_slot()`.
- `Renderer::track_count` and `Renderer::first_active_track` now delegate to the manager instead of owning loops.
- Added native coverage for empty, sparse, stopped, and cleared manager query states.
- Verified with native-only tests plus rebuilt smoke and track remove/compact UI scripts.

### P63 - Renderer Track Metadata Snapshot Boundary

Status: done in Patch 63.

Goal:

- Move `track_infos()` metadata assembly out of `Renderer` into a track snapshot helper.
- Keep the public `TrackInfo` payload and runner MethodChannel map unchanged.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv`

Result:

- Added `TrackInfo` as a standalone payload header and `snapshot_track_infos` in `track_snapshot`.
- `Renderer::track_infos` now delegates metadata assembly while preserving the public payload.
- Added native coverage for synthetic metadata, real demux/decode metadata, and slot ordering.
- Verified with native-only tests plus rebuilt smoke and track remove/compact UI scripts.

### P64 - Renderer Track Perf Snapshot Boundary

Status: done in Patch 64.

Goal:

- Move per-track perf stat field assembly out of `Renderer::track_perf_stats`.
- Keep `Renderer` responsible for locking, shared FPS baseline timing, and public API shape.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `TrackPerfStats` as a standalone payload header and `snapshot_track_perf_stats` in `track_snapshot`.
- `Renderer::track_perf_stats` now delegates per-track payload assembly while retaining the public lock and shared FPS baseline timer.
- Added native coverage for buffer state, current-frame timestamps, average/max decode timing, FPS delta calculation, and short-window no-FPS behavior.
- Verified with native-only tests plus rebuilt smoke UI.

### P65 - Renderer Track GPU Memory Snapshot Boundary

Status: done in Patch 65.

Goal:

- Move per-track GPU/memory stats field assembly out of `Renderer::gpu_memory_stats`.
- Keep `Renderer` responsible for device/state locking, aggregate totals, D3D resource ownership, and public API shape.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `TrackGpuMemoryStats` as a standalone payload header and `snapshot_track_gpu_memory_stats` in `track_snapshot`.
- `Renderer::gpu_memory_stats` now delegates per-track buffer, packet, decode, exact-seek, and presenter-copy memory payload assembly while retaining device locks and aggregate totals.
- Added native coverage for synthetic track buffer bytes, packet queue bytes, decode memory fields, presenter-copy bytes, exact-seek counters, and no-decode defaults.
- Verified with native-only tests plus rebuilt smoke UI.

### P66 - Renderer Loop Range Seek Policy Boundary

Status: done in Patch 66.

Goal:

- Move `apply_loop_range_locked` decision logic into a small seek/playback policy helper.
- Keep `Renderer` responsible for lock ownership, reading the playback clock, logging, and invoking `seek_internal`.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/loop/h265_loop_range_enable_regression.csv`

Result:

- Added `LoopRangeSeekInput`, `LoopRangeSeekDecision`, and `choose_loop_range_seek` to `SeekCoordinator`.
- `Renderer::apply_loop_range_locked` now delegates the boundary-trigger decision while retaining clock reads, logging, and exact seek execution.
- Added native coverage for at-end/past-end loop seeks and disabled, paused, stopped, and invalid loop states.
- Verified with native-only tests plus rebuilt smoke and H.265 loop range enable UI.

### P67 - Renderer Loop Range State Boundary

Status: done in Patch 67.

Goal:

- Move `LoopRangeState` and loop range normalization/comparison out of `Renderer`.
- Keep `Renderer` responsible for validation, lock ownership, state storage, and debug logging.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/loop/h265_loop_range_enable_regression.csv`

Result:

- Added `LoopRangeState`, `normalize_loop_range_state`, and `loop_range_states_equal` to `SeekCoordinator`.
- `Renderer::set_loop_range` now delegates state normalization/comparison while retaining validation, locks, stored state, and logging.
- Added native coverage for enabled, disabled, invalid, equal, and different loop-range states.
- Verified with native-only tests plus rebuilt smoke and H.265 loop range enable UI.

### P68 - Renderer Playback Decode State Boundary

Status: done in Patch 68.

Goal:

- Move public play/pause track decode pause and pause-after-preroll fanout out of `Renderer`.
- Keep `Renderer` responsible for lifecycle/state locks, playback clock commands, `playing_`, and seek coordinator reset.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `TrackPlaybackDecodeStateHooks` and `apply_track_playback_decode_state` to `track_lifecycle`.
- `Renderer::play` and `Renderer::pause` now delegate track pause-after-preroll, decode pause, and audio decode pause fanout while retaining locks, playback commands, `playing_`, and seek coordinator reset.
- Preserved the previous fanout order: all pause-after-preroll updates, then all decode pause updates, then audio pause.
- Verified with native-only tests plus rebuilt smoke UI.

### P69 - Renderer Decode Pause Fanout Boundary

Status: done in Patch 69.

Goal:

- Move the remaining all-track decode/audio pause fanout in `Renderer::set_decode_paused_for_all_tracks` into `track_lifecycle`.
- Keep `Renderer` responsible for call-site intent and lock ownership.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `TrackDecodePauseHooks` and `apply_track_decode_pause_state` to `track_lifecycle`.
- `Renderer::set_decode_paused_for_all_tracks` now delegates remaining all-track decode/audio pause fanout while preserving call-site intent and lock ownership.
- `apply_track_playback_decode_state` reuses the same helper so play/pause and render-loop pause paths share the same decode/audio fanout order.
- Verified with native-only tests plus rebuilt smoke UI.

### P70 - Renderer Step Decode Pause Boundary

Status: done in Patch 70.

Goal:

- Move `Renderer::step_forward` temporary per-track decode pause/resume fanout into `track_lifecycle`.
- Preserve current step behavior: step-forward should only pause/resume video decode threads and must not touch audio decode pause state.
- Keep `Renderer` responsible for step decision, clock update, wait loop, and exact-seek fallback.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_step_forward_visual_regression.csv`

Result:

- Added `apply_track_video_decode_pause_state` as a video-only lifecycle helper layered on the existing decode pause fanout boundary.
- `Renderer::step_forward` now delegates the temporary decode resume/pause points while preserving the exact-seek fallback and leaving audio decode pause untouched.
- Added native coverage for the video-only fanout path and verified with rebuilt smoke plus H.265 step-forward UI.

### P71 - Renderer Step Buffering Gate Boundary

Status: done in Patch 71.

Goal:

- Move the duplicate step-forward/step-backward "any track is Buffering" gate out of `Renderer` into a small track lifecycle/query helper.
- Keep `Renderer` responsible for lifecycle/state locks, playback clock pause, step direction, frame selection, and fallback seek.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_step_forward_visual_regression.csv`

Result:

- Added `has_buffering_track` to `track_lifecycle`.
- `Renderer::step_forward` and `Renderer::step_backward` now share the same Buffering gate instead of duplicating track-buffer state loops.
- Added native coverage for Empty, Ready, Buffering, and Flushing states and verified with rebuilt smoke plus H.265 step-forward UI.

### P72 - Renderer Step Backward Retreat Boundary

Status: done in Patch 72.

Goal:

- Move `Renderer::step_backward` all-track `can_retreat()` and `retreat()` fanout into a track lifecycle helper.
- Keep `Renderer` responsible for lifecycle/state locks, playback clock pause, reference-track clock seek, fallback exact seek, and final paused-frame draw.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `retreat_tracks_if_all_can_retreat` to move the all-track `can_retreat()` / `retreat()` sequence behind one lifecycle helper.
- Preserved all-or-nothing behavior: no track is retreated unless every active track can retreat.
- `Renderer::step_backward` now keeps clock/fallback/draw decisions and delegates the per-track retreat fanout.
- Verified with native-only tests plus rebuilt smoke UI.

### P73 - Renderer Step Policy Owner Boundary

Status: done in Patch 73.

Goal:

- Move step-specific helpers out of generic `track_lifecycle` into a dedicated `track_step_policy` owner before step logic grows there.
- Keep public helper semantics unchanged: Buffering gate, video decode pause fanout, and backward retreat fanout should keep their existing tests.
- Keep `Renderer` call sites behavior-equivalent in this ownership-only patch.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `track_step_policy.h/.cpp` and wired it into `VOID_RENDERER_WINDOWS_SOURCES`.
- Moved the step Buffering gate, video-only decode pause fanout, and all-track backward retreat helper out of `track_lifecycle`.
- Updated tests and `Renderer` includes so step-specific behavior has a dedicated owner and `track_lifecycle` does not keep absorbing step policy.
- Verified with native-only tests plus rebuilt smoke UI.

### P74 - Renderer Step Forward Decision Boundary

Status: done in Patch 74.

Goal:

- Move `Renderer::build_step_forward_decision_locked` next-frame selection into `track_step_policy`.
- Move `Renderer::discard_step_forward_consumed_frames_locked` consumed-frame draining into `track_step_policy`.
- Keep `Renderer` responsible for lock ownership, playback clock updates, wait loop, exact-seek fallback, present, and logging.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_step_forward_visual_regression.csv`

Result:

- Added `build_step_forward_decision` and `discard_step_forward_consumed_frames` to `track_step_policy`.
- Removed the corresponding private Renderer methods; `Renderer::step_forward` now passes current clock, frame duration, last decision, and track manager facts to the policy helper.
- Added focused native coverage for empty decisions, last-decision base PTS, oversized step gaps, and consumed-frame draining.
- Verified with native-only tests plus rebuilt smoke and H.265 step-forward UI.

### P75 - Renderer Step Frame Duration Boundary

Status: done in Patch 75.

Goal:

- Move `Renderer::compute_frame_duration_us` min-current-frame duration policy into `track_step_policy`.
- Keep the fallback duration and "highest FPS wins" semantics unchanged.
- Keep `Renderer` responsible for deciding when step-forward/step-backward need a duration.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_step_forward_visual_regression.csv`

Result:

- Added `compute_min_current_frame_duration_us` to `track_step_policy`.
- Removed the private `Renderer::compute_frame_duration_us` method; step-forward, step-backward, and EOF settle tolerance now reuse the same track policy helper.
- Added native coverage for empty tracks, ignored non-positive durations, trusted minimum duration, and oversized-duration fallback.
- Verified with native-only tests plus rebuilt smoke and H.265 step-forward UI.

### P76 - Renderer Preroll Buffering Gate Boundary

Status: done in Patch 76.

Goal:

- Move render-loop preroll readiness scan (`Buffering` / `Empty` / `Flushing`) out of `Renderer`.
- Keep `Renderer` responsible for clock pause/resume, `was_buffering_`, preview invalidation, and logging.
- Do not mix this with paused preview snapshot or render-sink carry-forward changes.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `track_preroll_policy` with `has_preroll_blocking_track`.
- Render loop now delegates the Empty/Buffering/Flushing preroll readiness scan while keeping clock pause/resume, `was_buffering_`, preview invalidation, and logging in `Renderer`.
- Added native coverage for Ready, Empty, Buffering, Flushing, Error, and missing-buffer states.
- Verified with native-only tests plus rebuilt smoke UI.

### P77 - Renderer Paused Preview Snapshot Boundary

Status: done in Patch 77.

Goal:

- Move paused preview snapshot assembly (`ALL active tracks have frames` rule) out of the render loop into a small policy/helper.
- Keep `Renderer` responsible for cached last-frame reuse, present, `preview_drawn_`, and logging.
- Preserve the rule that partial active-track previews do not draw, avoiding black flashes during seek/preroll.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `track_preview_policy` with `build_paused_preview_snapshot`.
- Render loop now delegates paused preview snapshot assembly while keeping cached last-frame reuse, present, clock correction, `preview_drawn_`, seek-preview events, and logging in `Renderer`.
- Added native coverage for empty managers, all-ready previews, Ready-at-EOF tracks, Buffering tracks with/without frames, and missing buffers.
- Verified with native-only tests plus rebuilt smoke UI.

### P78 - Renderer Present Carry-Forward Boundary

Status: done in Patch 78.

Goal:

- Move playing-state `PresentDecision` carry-forward logic out of the render loop.
- Preserve the rule that active tracks can reuse their last frame only after their effective PTS is non-negative, so shorter tracks freeze at EOF while longer tracks continue.
- Keep `Renderer` responsible for calling `RenderSink::evaluate`, `present_frame`, `last_decision_` assignment, and layout redraw fallback.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `track_present_policy` with `apply_present_carry_forward`.
- Render loop now delegates missing-frame carry-forward while keeping `RenderSink::evaluate`, `present_frame`, `last_decision_` commit, and layout redraw fallback in `Renderer`.
- Added native coverage for active carry-forward, negative effective PTS blocking, new-frame preservation, and inactive-track rejection.
- Verified with native-only tests plus rebuilt smoke UI.

### P79 - Renderer Empty-Buffer EOF Clamp Boundary

Status: done in Patch 79.

Goal:

- Move render-loop empty-buffer scan and max last-presented end-PTS calculation out of `Renderer`.
- Keep `Renderer` responsible for clock seek/clamp and `settle_eof_locked`.
- Preserve the behavior that one non-empty active buffer disables the EOF clamp.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `compute_empty_buffer_eof_clamp` to `track_present_policy`.
- Render loop now delegates the empty-buffer/max-end-PTS scan while keeping clock clamping and `settle_eof_locked` in `Renderer`.
- Added native coverage for empty managers, all-empty buffers, non-empty buffer disabling, and missing-buffer handling.
- Verified with native-only tests plus rebuilt smoke UI.

### P80 - Renderer Frame Deadline Event Boundary

Status: done in Patch 80.

Goal:

- Move render-loop next frame event PTS scan out of `Renderer`.
- Keep `Renderer` responsible for reading clock state, applying playback speed, calling `RenderLoopController::frame_deadline_sleep`, and sleeping.
- Preserve the rule that future frames wake at their PTS and current frames wake at PTS + duration.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `compute_next_frame_event_pts_us` to `track_present_policy`.
- Render loop now delegates next frame event PTS scanning while keeping clock reads, speed, `RenderLoopController::frame_deadline_sleep`, and actual sleep in `Renderer`.
- Added native coverage for no-frame managers, future-frame wakeups, current-frame expiry wakeups, empty buffers, and missing buffers.
- Verified with native-only tests plus rebuilt smoke UI.

### P81 - Renderer Diagnostics Snapshot Boundary

Status: done in Patch 81.

Goal:

- Move periodic render-loop track diagnostics snapshot assembly out of `Renderer`.
- Keep `Renderer` responsible for diagnostics cadence (`RenderLoopController`) and emitting the existing log lines.
- Preserve logged payload fields: slot, PTS, PTS delta, buffer count/capacity, buffer state, and playing snapshot.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `snapshot_render_loop_track_diagnostics` to `track_snapshot`.
- Render loop now delegates per-track diagnostics data collection while keeping cadence and log emission in `Renderer`.
- Added native coverage for empty managers, buffered tracks, and missing-buffer tracks.
- Verified with native-only tests plus rebuilt smoke UI.

### P82 - Renderer Paused Frame Draw Snapshot Boundary

Status: done in Patch 82.

Goal:

- Move `draw_paused_frame()` current-frame snapshot assembly out of `Renderer`.
- Keep `Renderer` responsible for last-decision fallback, present, clock/log reference selection, and `last_decision_` commit.
- Preserve the behavior that this draw helper uses any available active track frame and does not require all active tracks to be ready.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `build_available_paused_frame_snapshot` to `track_preview_policy`.
- `Renderer::draw_paused_frame` now delegates available current-frame snapshot assembly while keeping last-decision fallback, present, reference-slot logging, and `last_decision_` commit in `Renderer`.
- Added native coverage for empty managers, partial active-track frames, tracks without buffers, and non-Ready frames.
- Verified with native-only tests plus rebuilt smoke UI.

### P83 - Renderer Layout Track Geometry Snapshot Boundary

Status: done in Patch 83.

Goal:

- Move `snapshot_layout_track_geometry` out of `renderer.cpp` and into the layout-owned helper module.
- Keep `Renderer` responsible for passing the current track manager to shader constant population.
- Preserve active slot, width, height, and aspect semantics.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/viewport_pan_layout_regression.csv`

Result:

- Moved `snapshot_layout_track_geometry` into `layout_geometry`.
- Removed the anonymous layout snapshot helper from `renderer.cpp`.
- Added native coverage for inactive slots and active slot width/height/aspect snapshots.
- Verified with native-only tests plus rebuilt smoke and viewport pan/layout UI.

### P84 - Renderer Initial Render-Sink Binding Boundary

Status: done in Patch 84.

Goal:

- Move initial active-track-to-`RenderSink` binding out of `Renderer::initialize`.
- Keep `Renderer` responsible for constructing the `RenderSink` with the playback clock.
- Preserve slot-to-track-buffer binding semantics.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `bind_existing_tracks_to_render_sink` to `track_lifecycle`.
- `Renderer::initialize` now constructs the `RenderSink` and delegates active slot binding.
- Added native coverage using a real `RenderSink::evaluate` call.
- Verified with native-only tests plus rebuilt smoke UI.

### P85 - Renderer Initial Layout Order Boundary

Status: done in Patch 85.

Goal:

- Move `Renderer::initialize` initial layout track append loop into `LayoutController`.
- Keep `Renderer` responsible for resetting layout state and owning the track manager.
- Preserve file-id insertion order and slot order semantics.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/viewport_pan_layout_regression.csv`

Result:

- Added `LayoutController::append_tracks`.
- `Renderer::initialize` now resets layout and delegates active track file-id/slot append to `LayoutController`.
- Added native coverage for slot-order layout state and public file-id snapshot order.
- Verified with native-only tests plus rebuilt smoke and viewport pan/layout UI.

### P86 - Renderer Initial Active-Track Query Boundary

Status: done in Patch 86.

Goal:

- Remove the remaining ad hoc active-track scan from `Renderer::initialize`.
- Reuse `TrackPipelineManager` as the owner of active-track count queries.
- Preserve the existing "no valid tracks" failure behavior.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Replaced the manual active-track scan in `Renderer::initialize` with `tracks_.count()`.
- Reused existing `TrackPipelineManager` active-query coverage.
- Verified with native-only tests plus rebuilt smoke UI.

### P87 - Renderer Perf Baseline Reset Boundary

Status: done in Patch 87.

Goal:

- Centralize initialization and teardown resets for perf baseline state.
- Keep public `track_perf_stats()` behavior unchanged.
- Avoid widening the track snapshot module until the mutable FPS baseline policy can be isolated cleanly.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `TrackPerfBaselineTracker` to own the stats timer and per-slot frame baselines.
- `Renderer` now resets/rotates/query-baselines through the tracker instead of holding raw baseline arrays.
- Added focused native coverage for reset and rotation thresholds.
- Verified with native-only tests plus rebuilt smoke UI.

### P88 - Renderer Initial Track Creation Boundary

Status: done in Patch 88.

Goal:

- Move the `Renderer::initialize` initial video-path loop into a track lifecycle helper.
- Keep `Renderer` responsible for supplying pipeline factory hooks, file-id allocation, and error logging context.
- Preserve max-track skip behavior, failed-pipeline skip behavior, and started-track slot order.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `open_initial_track_pipelines` with hook-provided pipeline creation, file-id allocation, and start hooks.
- `Renderer::initialize` now delegates initial video-path open/start/slot insertion to track lifecycle.
- Added focused native coverage for slot-order open and full-slot skip behavior.
- Verified with native-only tests plus rebuilt smoke UI.

### P89 - Renderer Shutdown Resource Presence Boundary

Status: done in Patch 89.

Goal:

- Centralize the shutdown "do we have anything to release?" predicate.
- Reuse track manager active-track queries instead of a local track scan.
- Keep shutdown idempotency and early-return behavior unchanged.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `TrackPipelineManager::has_active_tracks`.
- `Renderer::shutdown` now delegates resource-presence checks to `has_resources_locked()`.
- `Renderer::initialize` uses the active-track predicate for the no-valid-tracks failure.
- Verified with native-only tests plus rebuilt smoke UI. One first native-only run hit an unrelated `analysis_tests` read-count flake; immediate rerun passed.

### P90 - Renderer Present-Decision Frame Query Boundary

Status: done in Patch 90.

Goal:

- Move the pure `PresentDecision` frame-presence query out of `Renderer`.
- Keep render-loop fallback behavior unchanged.
- Add focused policy coverage for empty and populated decisions.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `present_decision_has_frame` to `track_present_policy`.
- Removed `Renderer::has_any_frame` and switched render-loop fallback paths to the policy helper.
- Added focused native coverage for empty and populated decisions.
- Verified with native-only tests plus rebuilt smoke UI.

### P91 - Preview Policy Present-Decision Query Reuse

Status: done in Patch 91.

Goal:

- Remove the duplicate anonymous frame-presence helper from `track_preview_policy`.
- Reuse `track_present_policy` for the shared `PresentDecision` query.
- Keep paused preview readiness behavior unchanged.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Removed the anonymous `has_any_frame` helper from `track_preview_policy`.
- Paused preview readiness now reuses `present_decision_has_frame`.
- Verified with native-only tests plus rebuilt smoke UI.

### P92 - Renderer Effective Duration Policy Boundary

Status: done in Patch 92.

Goal:

- Move effective playback-duration synthesis out of `Renderer::effective_duration_us_locked`.
- Reuse track duration facts from lifecycle/policy helpers.
- Preserve cached-duration fallback when no track exposes a usable end PTS.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `resolve_effective_duration_us` to track lifecycle policy.
- `Renderer::effective_duration_us_locked` now delegates to the policy helper.
- Added focused native coverage for cached fallback and track end-PTS plus offset synthesis.
- Verified with native-only tests plus rebuilt smoke UI.

### P93 - Renderer Track Geometry Update Boundary

Status: done in Patch 93.

Goal:

- Move `Renderer::update_track_geometry_from_decision_locked` frame-size/aspect mutation logic into a layout/geometry helper.
- Keep `Renderer` responsible for logging geometry changes.
- Preserve SAR carry-forward behavior and invalid frame geometry filtering.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/viewport_pan_layout_regression.csv`

Result:

- Added `update_layout_track_geometry_from_decision` and `LayoutTrackGeometryUpdate`.
- `Renderer::update_track_geometry_from_decision_locked` now delegates frame-size/aspect mutation and only logs returned updates.
- Added native coverage for invalid frame filtering, SAR carry-forward, update records, and unchanged geometry suppression.
- Verified with native-only tests plus rebuilt smoke and viewport pan/layout UI.

### P94 - Renderer Cached Present PTS Query Boundary

Status: done in Patch 94.

Goal:

- Move the cached paused-frame first-PTS scan out of `Renderer::run`.
- Reuse a focused `PresentDecision` query helper next to other present policy helpers.
- Keep paused-frame logging behavior unchanged while removing another anonymous scan from the Renderer godobject.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `first_present_decision_frame_pts_us` to `track_present_policy`.
- Removed the anonymous `last_decision_.frames` scan from the paused cached-frame logging path.
- Extended native present policy coverage for empty decisions and slot-order PTS selection.
- Verified with native-only tests plus rebuilt smoke UI.

### P95 - Renderer Seek Preview Event Boundary

Status: done in Patch 95.

Goal:

- Move seek-preview presented track-event collection out of `Renderer::emit_seek_preview_presented_events`.
- Keep Renderer responsible for pending seek-event state and callback emission.
- Reuse track file-id and `PresentDecision` data through a focused present policy helper.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/timeline/h265_timeline_click_like.csv`

Result:

- Added `SeekPreviewPresentedTrackEvent` and `collect_seek_preview_presented_track_events` to `track_present_policy`.
- `Renderer::emit_seek_preview_presented_events` now handles pending request state and emits converted `RendererEvent` records.
- Added native coverage for slot ordering, missing-track filtering, invalid file-id filtering, request/target propagation, and DTS passthrough.
- Verified with native-only tests plus rebuilt smoke and h265 timeline click-like UI.

### P96 - Renderer Track Perf Stats Snapshot Boundary

Status: done in Patch 96.

Goal:

- Move the per-track perf stats scan out of `Renderer::track_perf_stats`.
- Keep Renderer responsible for timer/baseline ownership, but move active-track snapshot collection into `track_snapshot`.
- Preserve baseline rotation behavior and current-frame reporting.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `TrackPerfStatsCollectionResult` and `snapshot_track_perf_stats_collection` to `track_snapshot`.
- `Renderer::track_perf_stats` now delegates active-track stats collection and only handles elapsed time plus baseline rotation.
- Added native coverage for slot-ordered collection, buffer/current-frame fields, and decoded-frame baseline update data.
- Verified with native-only tests plus rebuilt smoke UI.

### P97 - Renderer Track GPU Memory Stats Snapshot Boundary

Status: done in Patch 97.

Goal:

- Move the per-track GPU/memory stats scan out of `Renderer::gpu_memory_stats`.
- Keep Renderer responsible for D3D presenter/headless/overlay aggregate resources.
- Reuse `snapshot_track_gpu_memory_stats` through a collection helper that returns aggregate track memory totals.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `TrackGpuMemoryStatsCollectionResult` and `snapshot_track_gpu_memory_stats_collection` to `track_snapshot`.
- `Renderer::gpu_memory_stats` now delegates active-track memory stats collection and aggregate per-track totals.
- Added native coverage for slot-ordered collection, presenter copy bytes, buffer/packet totals, and aggregate CPU/estimated totals.
- Verified with native-only tests plus rebuilt smoke UI.

### P98 - Renderer Analysis Overlay Memory Stats Boundary

Status: done in Patch 98.

Goal:

- Move analysis-overlay GPU resource memory aggregation out of `Renderer::gpu_memory_stats`.
- Keep Renderer responsible for owning device locks and merging returned aggregate bytes/dimensions.
- Cover rect-capacity accounting without requiring live D3D texture allocation.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Added `AnalysisOverlayMemoryStats` and `snapshot_analysis_overlay_memory_stats` to `analysis_overlay_renderer`.
- `Renderer::gpu_memory_stats` now delegates overlay rect/texture memory accounting and only merges returned bytes/dimensions.
- Added native coverage for overlay rect-buffer memory accounting without requiring live D3D texture allocation.
- Verified with native-only tests plus rebuilt smoke UI.

### P99 - Renderer Seek Track Facts Boundary

Status: done in Patch 99.

Goal:

- Move per-track seek classification facts out of `Renderer::seek_internal`.
- Centralize target clamp, hardware-decode status, HEVC hardware seek detection, and H.264/FLV exact-seek warning facts.
- Keep Renderer responsible for logging, hook wiring, pipeline recreation, and seek submission for this patch.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `TrackSeekFacts`, `inspect_track_seek_facts`, `track_uses_hardware_codec`, and `any_track_uses_hardware_codec` to `track_lifecycle`.
- `Renderer::seek_internal` now consumes per-track seek facts instead of directly inspecting demux/decode details.
- `Renderer::has_hevc_hw_track_locked` now delegates the HEVC hardware scan to `track_lifecycle`.
- Added native coverage for target resolution, no-demux/no-decode facts, and empty-manager hardware codec scanning.
- Verified with native-only tests plus rebuilt smoke and h265 seek visual UI.

### P100 - Renderer Seek Track Transition Boundary

Status: done in Patch 100.

Goal:

- Continue reducing `Renderer::seek_internal` by extracting per-track transition/recreate input assembly.
- Keep Renderer responsible for hook wiring and actual `recreate_pipeline_for_seek` / `submit_track_seek_after_recreate` calls.
- Preserve paused HEVC hardware seek coalescing and failure behavior.

Result:

- Added `TrackSeekTransitionPlan` and `build_track_seek_transition_plan()` to `track_lifecycle`.
- `Renderer::seek_internal()` now delegates paused/type/HEVC recreate input assembly and keeps hook wiring, recreate, submit, and logging.
- Added native coverage for paused and playing plan field propagation.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

### P101 - Renderer Seek Track Execution Boundary

Status: done in Patch 101.

Goal:

- Continue reducing `Renderer::seek_internal()` by extracting the per-track post-decision execution boundary around HEVC recreate application, error/coalesce result handling, and seek submission.
- Keep Renderer responsible for member-capturing hook wiring and top-level pending preview/global clock state.
- Preserve paused HEVC recreate failure and coalescing behavior exactly.

Result:

- Added `TrackSeekExecutionResult` and `apply_track_seek_execution_result()` to `track_lifecycle`.
- `Renderer::seek_internal()` now refreshes the slot after optional recreate and delegates error-state/seek-submit result handling.
- Added native coverage for normal submit, failed recreate error state, coalescing metadata, and recreated seek submission suppression.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

### P102 - Renderer Seek Slot Application Boundary

Status: done in Patch 102.

Goal:

- Move the remaining per-slot seek orchestration inside `Renderer::seek_internal()` into a track-lifecycle helper that owns facts inspection, transition preparation, plan construction, recreate decision, and execution result assembly.
- Keep Renderer responsible for building member-capturing hooks, top-level pending preview/global clock state, and final logging/callback-visible side effects.
- Preserve all seek logs and paused HEVC recreate behavior.

Result:

- Added `TrackSeekSlotApplicationHooks`, `TrackSeekSlotApplicationResult`, and `apply_track_seek_to_slot()` to `track_lifecycle`.
- `Renderer::seek_internal()` now delegates per-slot seek facts, preparation, plan, HEVC recreate decision, and execution result assembly.
- Added native coverage for empty-slot no-op and active-slot seek target/flush/pending-seek behavior.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

### P103 - Renderer Step Forward Fallback Boundary

Status: done in Patch 103.

Goal:

- Move `Renderer::step_forward()` exact-seek fallback target calculation into `track_step_policy`.
- Keep Renderer responsible for the wait loop, playback clock mutation, seek execution, draw/log calls, and lifecycle locking.
- Preserve cache-miss step-forward target clamping and log values.

Result:

- Added `StepForwardExactSeekTarget` and `choose_step_forward_exact_seek_target()` to `track_step_policy`.
- `Renderer::step_forward()` now delegates fallback base/duration/target calculation and keeps wait-loop/seek/log ownership.
- Added native coverage for empty-track fallback, peek-frame fallback, last-decision priority, and duration clamp behavior.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_step_forward_visual_regression.csv`

### P104 - Renderer Step Backward Fallback Boundary

Status: done in Patch 104.

Goal:

- Move `Renderer::step_backward()` cache-miss exact-seek target calculation into `track_step_policy`.
- Keep Renderer responsible for retreat execution, seek invocation, draw/log calls, and lifecycle locking.
- Preserve the 1ms backward margin and zero clamp behavior.

Result:

- Added `StepBackwardExactSeekTarget` and `choose_step_backward_exact_seek_target()` to `track_step_policy`.
- `Renderer::step_backward()` now delegates fallback duration/target calculation and keeps retreat/seek/log ownership.
- Added native coverage for fallback duration, zero clamp, and non-clamped target behavior.
- Added `ui_tests/seek/h265_seek_step_backward_visual_regression.csv` to cover visible step-backward behavior.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_step_backward_visual_regression.csv`

### P105 - Renderer Step Forward Decision Application Boundary

Status: done in Patch 105.

Goal:

- Continue shrinking `Renderer::step_forward()` by extracting the repeated successful-decision application facts around consumed-frame discard, reference slot selection, and clock target calculation.
- Keep Renderer responsible for lifecycle locking, wait-loop timing, `present_frame()`, and final seek/draw/log calls.
- Preserve step-forward presentation and exact-seek fallback behavior.

Result:

- Added `StepForwardDecisionApplication` and `apply_step_forward_decision()` to `track_step_policy`.
- `Renderer::step_forward()` now delegates consumed-frame discard, reference-slot selection, and successful-decision clock target calculation.
- Added native coverage for reference slot selection, clock target calculation, and consumed-frame discard.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_step_forward_visual_regression.csv`

### P106 - Renderer Step Backward Retreat Application Boundary

Status: done in Patch 106.

Goal:

- Move `Renderer::step_backward()` successful retreat clock target calculation into `track_step_policy`.
- Keep Renderer responsible for lifecycle locking, fallback seek execution, draw/log calls, and final paused-frame presentation.
- Preserve retreat success behavior and existing step-backward fallback behavior.

Result:

- Added `StepBackwardRetreatApplication` and `choose_step_backward_retreat_application()` to `track_step_policy`.
- `Renderer::step_backward()` now delegates retreat-success reference-slot and clock-target calculation.
- Added native coverage for empty, active-frame, and missing-frame retreat application cases.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_step_backward_visual_regression.csv`

### P107 - AudioEngine Track Registry Boundary

Status: done in Patch 107.

Goal:

- Start addressing `review_godobject.md`'s `AudioEngine::Impl` finding by extracting track registry/query mutation policy out of the implementation body.
- Keep waveOut/device submission, decoder thread lifecycle, and mixer behavior unchanged.
- Preserve pause/no-PCM-consumption behavior and active-track selection semantics.

Result:

- Added `AudioTrackRegistry` and `AudioTrackController` to own audio track storage, buffer publication maps, replacement/removal/clear handles, and pause/seek fanout.
- `AudioEngine::Impl` now delegates track map mutation and query behavior to the registry while keeping playback state, output device ownership, and decoder construction.
- Added focused registry tests covering buffer publication, pause/seek fanout, remove/clear ownership, and replacement behavior.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

### P108 - AudioEngine Decode Thread Boundary

Status: done in Patch 108.

Goal:

- Continue addressing `review_godobject.md`'s `AudioEngine::Impl` finding by moving the nested `AudioDecodeThread` implementation out of `audio_engine.cpp`.
- Keep `AudioEngine::Impl` as the audio coordinator and preserve the newly extracted `AudioTrackRegistry` ownership boundary.
- Preserve pause/no-PCM-consumption, seek flush, resampler setup, and FFmpeg decoder lifecycle behavior.

Result:

- Added `AudioDecodeThread` as a dedicated audio module instead of a nested `audio_engine.cpp` implementation class.
- Added shared audio output constants so the decoder, PCM buffer creation, mixer, and waveOut format do not duplicate sample-rate/channel parameters.
- `AudioEngine::Impl` now constructs the decoder through the dedicated boundary and keeps only coordinator-level ownership.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

### P109 - AudioEngine WaveOut Output Boundary

Status: done in Patch 109.

Goal:

- Continue addressing `review_godobject.md`'s `AudioEngine::Impl` finding by moving the nested waveOut device/output thread implementation out of `audio_engine.cpp`.
- Keep `AudioMixer` behavior, pause/no-PCM-consumption, and active-track transition semantics unchanged.
- Leave `AudioEngine::Impl` responsible for play/pause policy and track registry coordination.

Result:

- Added `WaveOutOutput` as a dedicated audio output/device module around the WinMM buffer submission thread.
- `audio_engine.cpp` no longer owns WinMM headers, waveOut device state, audio sample buffer submission, or mixer internals.
- `AudioEngine::Impl` is now a small coordinator over `AudioTrackRegistry`, `AudioDecodeThread`, and `WaveOutOutput`.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

### P110 - AnalysisManager Session Boundary

Status: done in Patch 110.

Goal:

- Resume `review_godobject.md`'s `AnalysisManager` finding by extracting the next session/registry/cache-facing boundary that still lives in the singleton manager.
- Preserve existing analysis FFI/session snapshot behavior and overlay cache correctness fixed in the overlay rounds.
- Choose the exact patch boundary after re-reading current `analysis_manager.*`, because several earlier overlay fixes already moved part of the state model.

Result:

- Added `AnalysisSession` to own VAC2 base data, frame summary queries, PTS-to-frame mapping, overlay chunk index refresh, single-frame cache, and decoded chunk LRU.
- `AnalysisManager` now holds an immutable session snapshot and delegates session reads to `AnalysisSession`.
- Existing overlay chunk filtering/cache behavior and concurrent manager load/unload/read tests continue to cover the boundary.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv`

### P111 - AnalysisManager Overlay Track Registry Boundary

Status: done in Patch 111.

Goal:

- Continue reducing `AnalysisManager` by extracting overlay track registration/snapshot storage out of the singleton manager.
- Prefer storing per-track `AnalysisSession` snapshots rather than recursive `AnalysisManager` instances if the current call sites allow it.
- Preserve renderer-facing `overlay_track_snapshot()` behavior and FFI `set_overlay_track` / `clear_overlay_tracks` semantics.

Result:

- Added `AnalysisOverlayTrackRegistry` for overlay track set/clear/snapshot storage.
- Overlay track snapshots now return `AnalysisSession` objects directly, and `AnalysisOverlayRenderer` reads per-track overlay data without recursive `AnalysisManager` instances.
- Added native FFI coverage confirming overlay set/clear publishes readable per-track session snapshots.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv`

### P112 - DecodeThread Codec Loop Boundary

Status: done in Patch 112.

Goal:

- Resume `review_godobject.md`'s `DecodeThread` finding by extracting the next codec send/receive or frame ownership boundary from the main decode loop.
- Preserve exact-seek, pause-after-preroll, EOF drain, and hardware visibility behavior already covered by focused policy tests.
- Pick the exact cut after re-reading current `decode_thread.*`, because several seek/pause policies have already been split.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `codec_loop` for FFmpeg codec send/receive SEH wrappers, result classification, and optional hardware device mutex locking.
- `DecodeThread` now delegates codec send/receive calls in normal decode, drain-before-next-packet, EOF drain, and exact-seek drain paths.
- Added native coverage for codec result classification, including EAGAIN, EOF, SEH sentinel, and hard errors.

### P113 - DecodeThread Frame Publish Boundary

Status: done in Patch 113.

Goal:

- Continue reducing `DecodeThread` by extracting the next frame ownership/publish boundary after codec receive.
- Keep exact-seek candidate collection, pause-after-preroll, and hardware visibility flush semantics unchanged.
- Prefer a focused helper around frame rescale/log/visibility/convert/push if the current loop allows a narrow cut.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `DecodedFramePublisher` for hardware visibility flush, frame conversion, output-buffer publish, and conversion-failure state transitions.
- `DecodeThread` now delegates normal decode, EOF drain, drain-before-next-packet, reorder flush, and pending exact-seek frame publish paths to the publisher.
- Added native coverage for successful software-frame publish and conversion-failure Error/pause/stop handling.

### P114 - DecodeThread Drain Boundary

Status: done in Patch 114.

Goal:

- Continue reducing `DecodeThread` by extracting EOF drain and drain-before-next-packet loop mechanics once codec calls and frame publishing are already isolated.
- Keep exact-seek EOF drain and reorder fallback behavior unchanged.
- Prefer a narrow helper around drain loop orchestration before touching broader AVFrame lifetime.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `decode_drain_policy` for drain-before-next-packet receive actions, EOF codec-drain send/receive actions, and drain stop gates.
- `DecodeThread` now delegates drain request clearing, SEH error marking choice, EOF send handling, and pause/flush stop checks to the policy helpers.
- Added native coverage for EAGAIN, EOF, hard error, SEH sentinel, cancel, pause, and flushing drain decisions.

### P115 - DecodeThread Exact-Seek Candidate Ownership Boundary

Status: done in Patch 115.

Goal:

- Continue reducing `DecodeThread` by isolating exact-seek candidate ownership and memory counter maintenance from the main decode object.
- Keep preview-window selection and post-target pending-frame behavior unchanged.
- Prefer moving candidate collection/snapshot/memory stats behind a small owner if current call sites permit it.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `ExactSeekCandidateStore` for exact-seek candidate cloning, reorder/pending ownership, snapshot trigger points, preview-window readiness, and candidate memory stats.
- `DecodeThread` now delegates candidate collection, pending queue moves/pops, reorder counts, and memory stats snapshots to the store.
- Added native coverage for latest pre-target retention, first post-target snapshot trigger, pending tail moves, and stable-frame stats.

### P116 - DecodeThread Timestamp Rescale Boundary

Status: done in Patch 116.

Goal:

- Continue reducing `DecodeThread` by extracting stream-timebase-to-microseconds frame timestamp rescale from the decode loop lambda.
- Keep exact-seek target comparison and published PTS/DTS/duration semantics unchanged.
- Prefer a small tested helper so future drain/publish extraction does not carry a member-capturing timestamp lambda.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `frame_timestamp_rescaler` for AVFrame PTS, best-effort PTS, DTS, and duration conversion from stream time base to microseconds.
- Replaced the decode-loop timestamp lambda with the helper, leaving call sites and ordering unchanged.
- Added native coverage for direct PTS, best-effort fallback, missing DTS, and non-positive duration behavior.

### P117 - DecodeThread Seek Epoch Boundary

Goal:

- Continue reducing `DecodeThread` by extracting the seek notification take/reset/start-state preparation block from the main loop.
- Keep codec flush, exact-seek target setup, post-seek preroll, and cancellation semantics unchanged.
- Prefer a tested policy/helper for seek epoch state updates before moving broader EOF or frame lifetime choreography.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `decode_seek_epoch` for pending seek take/reset, seek type labeling, and exact/keyframe seek epoch start-state decisions.
- Replaced the inline `DecodeThread::run()` seek block with `take_pending_seek_notification()` and `begin_seek_epoch()`, preserving codec flush, exact seek target setup, post-seek fast preroll, cancellation reset, and Buffering transition ordering.
- Added native coverage for pending/idle/invalid seek notifications and exact/keyframe start-state differences.

### P118 - DecodeThread Packet Consumption Boundary

Goal:

- Continue reducing `DecodeThread` by extracting packet-consumption decisions around pause, flushing, stale seek packets, codec send action, and AVPacket ownership cleanup.
- Keep codec send ordering, cancellation semantics, queue EOF handling, and output state transitions unchanged.
- Prefer tested policy/helper seams before moving any AVPacket lifetime code out of the decode loop.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Extended `decode_loop_policy` with packet pop routing, cancel-before-send checkpoint, and packet send return-value decisions.
- Replaced the inline packet gap/stale packet/send error branches in `DecodeThread::run()` with tested policy calls while keeping AVPacket free timing, logs, and Error/paused/running state writes local.
- Added native coverage for packet-present vs queue-gap handling, cancelled non-packet sleeps, send EAGAIN/EOF receive-loop preservation, hard send errors, and SEH stop behavior.

### P119 - DecodeThread Receive Action Boundary

Goal:

- Continue reducing `DecodeThread` by extracting normal receive-loop return-value decisions around cancelled loops, codec receive SEH, EAGAIN/EOF, hard receive errors, and frame publish continuation.
- Keep AVFrame unref timing, exact-seek candidate capture, hardware visibility flush, preroll state transition, and perf counter updates unchanged.
- Prefer a tested policy/helper before moving frame lifetime or exact-seek publish code.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Extended `decode_loop_policy` with receive-loop cancel, EAGAIN/EOF, logged hard-error, and SEH error actions.
- Replaced the normal decode receive-loop return-value condition tree with tested policy calls while keeping AVFrame unref timing, exact-seek capture, publish, and preroll transitions local.
- Added native coverage for receive publish/stop/logged-error/error-stop behavior.

### P120 - DecodeThread Preroll Transition Boundary

Goal:

- Continue reducing duplicated `DecodeThread` Buffering -> Ready transition code around normal preroll, post-seek fast preroll, EOF preroll completion, and pause-after-preroll.
- Keep output buffer state mutation, `post_seek_` reset, `pause_after_preroll_` handling, and logs semantically unchanged.
- Prefer a tested helper/policy that computes transition intent before any broader frame publishing extraction.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `decode_preroll_policy` for post-seek software/hardware preroll targets, normal/full-preroll readiness, and Buffering->Ready transition intent.
- Replaced the duplicated normal preroll completion blocks with `DecodeThread::complete_preroll_if_ready()`, leaving logs, `TrackBuffer` state writes, `pause_after_preroll_`, and `post_seek_` reset in the decode thread.
- Added native coverage for hardware post-seek extra-frame readiness, normal preroll delegation, Ready transition decisions, and no-op states.

### P121 - DecodeThread EOF Drain Boundary

Goal:

- Continue reducing `DecodeThread` by extracting EOF/gap handling around Buffering exact-seek EOF drain, Buffering non-exact EOF mark-flushed, normal codec EOF drain, and post-seek EOF completion.
- Keep codec drain ordering, exact-seek reorder publish behavior, output state transitions, and logs unchanged.
- Prefer tested decisions before moving any codec drain or AVFrame lifetime code.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Moved queue-gap and EOF drain orchestration out of `DecodeThread::run()` into `handle_queue_gap_or_eof()`.
- Preserved existing tested EOF decisions from `decode_loop_policy` / `decode_drain_policy`, including Buffering exact-seek EOF drain, Buffering mark-flushed, normal codec drain, and send-error stop behavior.
- Kept codec drain ordering, exact-seek reorder publish calls, `eof_flushed_`, output state writes, and logs unchanged.

### P122 - DecodeThread Exact Seek Publish Boundary

Goal:

- Continue reducing `DecodeThread` by extracting exact-seek publish state transitions around selected preview publish, pending candidate publish, post-preview drain request, and pause-after-preroll.
- Keep AVFrame ownership, hardware visibility flush, stable-frame reuse, conversion failure handling, and preview logs unchanged.
- Prefer tested state decisions before moving any frame conversion or snapshot ownership code.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `exact_seek_publish_policy` for exact-seek preview publish-window sizing and post-preview completion state.
- `DecodeThread::publish_exact_seek_window()` now delegates output-buffer capacity/window math and Ready/pause/post-seek/target/drain-request completion intent while preserving frame conversion, stable-frame reuse, hardware wait/flush, candidate tail movement, and logs.
- Added native coverage for capacity-bounded preview windows, full/invalid output rejection, and completion state reset.

### P123 - Native GodObject Round Archive

Status: done in Patch 123.

Goal:

- Keep this current cockpit from growing into another historical log by moving older completed patch details into `NATIVE_STABILIZATION_HISTORY.md`.
- Preserve active status, still-active GodObject list, and the most recent/current patch queue needed for the next coding rounds.
- No product code changes.

Validation:

- Documentation-only; run `git diff --check`.

Result:

- Archived completed patch details P100-P115 into `NATIVE_STABILIZATION_HISTORY.md`.
- Kept the active cockpit focused on the still-open GodObject risks and recent DecodeThread slices.
