# Native Stabilization Round

本文件是 native 深度清理的当前驾驶舱。旧的 patch 流水账已归档到
`native/docs/NATIVE_STABILIZATION_HISTORY.md`，避免后续继续被 runner bridge cleanup 带偏。

## Sources

- `build/chat/review_native.md`
- `build/chat/review_overlay.md`
- `build/chat/review_godobject.md`
- `build/chat/review_renderer.md`
- `build/chat/review_renderer_v2.md`
- `build/chat/split_adv.md`
- `native/docs/NATIVE_REFACTOR_TODO.md`

## Focus Rule

当前优先级：

1. `review_renderer.md` 点名的 Renderer 并发状态模型和目录分层已完成；后续 Renderer 改动必须保持这些锁和目录契约。
2. `review_renderer_v2.md` 的新核验优先于普通 owner-boundary 拆分；先收真实的 Renderer 并发/生命周期缺口，再继续结构性下沉。
3. `review_godobject.md` 中 Renderer owner boundary 的后续拆分。
4. `ffi_exports.cpp` / `TrackPipelineManager` / `DecodeThread` 这些二级 God Module 的收缩，排在 Renderer 并发收敛之后。
5. `review_native.md` / `review_overlay.md` 已修 correctness 回归防线不能倒退。

暂时不要继续做纯 runner plugin 清理，除非它直接关闭 native owner boundary、process-global、测试隔离或上屏正确性问题。

每轮仍保持：一个问题一个 patch，测试通过后单独提交，本文档同步状态。

## Current Cross-Check

### `review_native.md`

Status: fixed.

Chat 列出的 13 个 correctness / lifecycle / validation 问题已经全部收掉：

- Demux seek callback race.
- RenderSink raw `TrackBuffer*` lifetime.
- Headless shared texture in-flight overwrite.
- Capture GPU wait while holding texture lock.
- Audio pause consuming PCM.
- NativePlayer facade lifecycle locking.
- FFI handle lease / long-operation serialization.
- Layout validation guardrails.
- RGBA texture dimension and stride guardrails.
- Demux read-error propagation.
- `avcodec_open2()` SEH guard.
- Odd-dimension software frame compatibility.
- D3D shutdown `ClearState + Flush`.

Regression coverage added:

- repeated create/destroy smoke.
- shutdown during real timeline seek + recreate smoke.
- native-only tests covering the lower-level guardrails.

### `review_overlay.md`

Status: fixed for the current chat audit.

Fixed:

- AnalysisManager lifecycle data race: session snapshots replace mutable singleton session reads.
- Overlay chunk consistency: chunk index filters by codec, base content revision, track index, and required CU geometry features.
- VACache publication: cache files are published through atomic replace without deleting the final path first, and tmp names are unique across path/PID/TID/counter/time tokens.
- VACHUNK hot-path IO/memory amplification: overlay chunks are decoded once into a small per-session LRU keyed by path and file metadata; adjacent frame reads slice cached decoded sections instead of re-opening and re-decoding every section.
- `overlay_raster.cpp` helper hardening: BGRA fill no longer writes through aliased `uint32_t*`, public raster helpers guard invalid surfaces, and heatmap output rejects overflow or oversized allocations before resizing.
- D3D overlay pass state contract: mask materialization now checks/restores the main RTV and viewport, unbinds overlay SRV hazards before writing mask RTVs, and overlay draw passes explicitly bind their render target, IA topology, shaders, blend state, and cleanup SRV slots.
- Overlay precision and opacity semantics: native regression tests now cover 1x1, 2x2, 8K, shared-boundary, and clamped packed-UV coordinates, and FFI overlay opacity preserves 0 instead of forcing 10%.
- Overlay generation budget and codec semantics: generated VACHUNK publish now uses the current remaining cache budget, VAC2/VACHUNK parsers reject undefined codec values, and overlay chunk keys are built from a validated base codec instead of a blind header cast.
- VACHUNK checksum fields: v1 semantics are now explicit reserved-zero fields, with parser validation for nonzero header/section checksum values and docs updated to match the external analyzer's current output.
- VACHUNK record-count guard: record section factories now check count/record-size narrowing before filling section metadata, and empty record sections no longer do pointer arithmetic on a null vector data pointer.
- VAC2 frame model boundary: the current one-packet-per-frame fallback is explicit in VAC2 metadata, frame/summary flags, format docs, and generator tests so overlay alignment assumptions are visible until exact AU grouping is implemented.

Active backlog:

- No open `review_overlay.md` items remain in this stabilization pass. Future work can replace the documented VAC2 fallback with exact access-unit grouping when the generator/analyzer exposes enough frame boundary data.

### `review_godobject.md`

Status: partially fixed, architecture backlog.

Fixed or reduced:

- `AudioEngine::Impl`: `AudioMixer` extracted and pause PCM consumption bug fixed.
- `AnalysisManager`: session snapshot split reduced global session risk.
- `Renderer`: analysis overlay CPU cache, D3D overlay resource helpers, mask pass, and overlay draw pass moved into `AnalysisOverlayRenderer`; `Renderer` now delegates the overlay pass after the base frame draw.
- `Renderer`: layout state/constants moved to layout-owned helpers; `Renderer` now snapshots track geometry and delegates shader layout math to `layout_geometry`.
- `Renderer`: layout track geometry snapshot helper moved into `layout_geometry`.
- `Renderer`: render-loop debounce, diagnostics cadence, and frame-deadline sleep policy moved into `RenderLoopController`.
- `Renderer`: remove-track stop/compact render-sink/presenter slot side effects and cached `PresentDecision` frame compaction moved into `track_lifecycle`.
- `Renderer`: add-track current-clock seek target clamp, buffer/queue flush, audio pause, and seek type choice moved into `track_lifecycle`.
- `Renderer`: HEVC hardware seek recreate/coalesce/error decision moved into `SeekCoordinator` policy.
- `Renderer`: generic per-track seek preparation and post-recreate seek submission moved into `track_lifecycle`.
- `Renderer`: seek pipeline stop/recreate/start/render-sink commit choreography moved into `track_lifecycle`; unused decode-thread-only recreate path removed.
- `Renderer`: add-track render-sink/frame-presenter/tracks slot commit moved into `track_lifecycle`.
- `Renderer`: initial active-track-to-RenderSink binding moved into `track_lifecycle`.
- `Renderer`: add/remove-track temporary playback pause, failure rollback, and remove-success resume policy moved into `track_lifecycle`.
- `Renderer`: seek target clamp and pending seek-preview event retarget decision moved into `SeekCoordinator` policy.
- `Renderer`: per-track seek target/offset clamp facts moved into `track_lifecycle`.
- `Renderer`: track offset mutation and render-sink offset synchronization moved into `track_lifecycle`.
- `Renderer`: active track count and first-active-slot queries moved into `TrackPipelineManager`.
- `Renderer`: track metadata snapshot assembly moved into `track_snapshot`.
- `Renderer`: periodic render-loop diagnostics snapshot assembly moved into `track_snapshot`.
- `Renderer`: per-track performance stats snapshot assembly moved into `track_snapshot`.
- `Renderer`: per-track GPU/memory stats snapshot assembly moved into `track_snapshot`.
- `Renderer`: loop-range boundary seek decision moved into `SeekCoordinator` policy.
- `Renderer`: loop-range state normalization and comparison moved into `SeekCoordinator` policy.
- `Renderer`: public play/pause decode pause and pause-after-preroll fanout moved into `track_lifecycle`.
- `Renderer`: remaining all-track decode/audio pause fanout moved into `track_lifecycle`.
- `Renderer`: step-forward temporary video decode pause/resume fanout moved into `track_lifecycle`.
- `Renderer`: shared step buffering gate moved into `track_lifecycle`.
- `Renderer`: step-backward retreat fanout moved into `track_lifecycle`.
- `Renderer`: step-backward retreat success reference-slot and clock-target calculation moved into `track_step_policy`.
- `Renderer`: step-specific track helpers moved into dedicated `track_step_policy` owner.
- `Renderer`: step-forward next-frame selection and consumed-frame draining moved into `track_step_policy`.
- `Renderer`: step-forward successful-decision application, reference slot, and clock target calculation moved into `track_step_policy`.
- `Renderer`: current-frame duration policy moved into `track_step_policy` and reused by step/EOF tolerance paths.
- `Renderer`: step-forward cache-miss exact-seek fallback target calculation moved into `track_step_policy`.
- `Renderer`: step-backward cache-miss exact-seek fallback target calculation moved into `track_step_policy`.
- `Renderer`: preroll readiness track-state scan moved into dedicated `track_preroll_policy` owner.
- `Renderer`: playing present-decision carry-forward moved into dedicated `track_present_policy` owner.
- `Renderer`: empty-buffer EOF clamp fact calculation moved into `track_present_policy`.
- `Renderer`: next frame deadline event PTS calculation moved into `track_present_policy`.
- `Renderer`: paused preview snapshot assembly moved into dedicated `track_preview_policy` owner.
- `Renderer`: paused-frame draw snapshot assembly moved into `track_preview_policy`.
- `Renderer`: initial layout track-order append moved into `LayoutController`.
- `Renderer`: initial active-track query now uses `TrackPipelineManager` ownership instead of an ad hoc scan.
- `Renderer`: perf baseline timer/frame reset state moved into `TrackPerfBaselineTracker`.
- `Renderer`: initial video-path open/start loop moved into `track_lifecycle`.
- `Renderer`: shutdown resource-presence predicate centralized and now uses `TrackPipelineManager` active-track query.
- `Renderer`: pure present-decision frame-presence query moved into `track_present_policy`.
- `track_preview_policy`: paused-preview readiness now reuses the shared present-decision frame query.
- `Renderer`: effective playback-duration synthesis moved into track lifecycle policy.
- `ffi_exports.cpp`: player handle registry, gate lease, thread-local last error, and per-player error state moved into `ffi_player_registry`.
- `ffi_exports.cpp`: ABI/config/log/layout/seek enum marshalling moved into `ffi_marshalling` with focused tests, leaving exported functions thinner.
- `ffi_exports.cpp`: playback/query/track/layout command bodies moved into `ffi_player_commands` with focused command tests.
- `TrackPipelineManager`: demux/decode pipeline construction moved into `TrackPipelineFactory`, leaving manager focused on slot storage, stop, and compact.
- `Renderer`: track pipeline metadata setup, callback/audio hook registration, demux start, and failed-start rollback moved into `track_lifecycle`.
- `Renderer`: track geometry mutation from presented frames moved into `layout_geometry`; Renderer now only logs returned geometry updates.
- `Renderer`: cached paused-frame first-PTS lookup moved into `track_present_policy`.
- `Renderer`: seek-preview presented track-event collection moved into `track_present_policy`; Renderer keeps pending-event state and callback emission.
- `Renderer`: per-track perf stats collection moved into `track_snapshot`; Renderer keeps timing and baseline rotation ownership.
- `Renderer`: per-track GPU/memory stats collection moved into `track_snapshot`; Renderer keeps D3D presenter/headless/overlay aggregation.
- `Renderer`: analysis-overlay GPU resource memory accounting moved into `analysis_overlay_renderer`.
- `Renderer`: per-track seek target/hardware/codec warning facts moved into `track_lifecycle`; Renderer keeps logging and seek actions.
- `Renderer`: per-track seek transition/recreate input assembly moved into `track_lifecycle`; Renderer keeps hook wiring and seek/recreate actions.
- `Renderer`: per-track seek execution result handling moved into `track_lifecycle`; Renderer refreshes the post-recreate slot and keeps logging.
- `Renderer`: per-slot seek facts/transition/plan/recreate-decision/execution orchestration moved into `track_lifecycle`; Renderer keeps global seek state, member-capturing hooks, and logs.
- `Renderer`: public play/pause/step command intent moved into `renderer_playback_command_policy`; Renderer keeps locks, playback clock ownership, and command execution.
- `Renderer`: global seek clock/deferred HEVC gate facts moved into `SeekCoordinator` policy; Renderer keeps clock mutation, coordinator state mutation, and seek execution.
- `Renderer`: seek diagnostics data assembly moved into `renderer_seek_log_policy`; Renderer keeps log emission timing and seek side effects.
- `Renderer`: resize-driven view-offset scaling moved into `layout_geometry`; Renderer keeps resize locks, target dimensions, and D3D output resize.
- `DecodeThread`: exact-seek lookbehind, preview-window readiness, and preview-frame selection now live in `exact_seek_window` with focused state tests.
- `DecodeThread`: pending exact-seek publish, drain-before-next-packet, paused consumption, and stale-packet discard guards now live in `decode_loop_policy` with focused state tests.
- `DecodeThread`: EOF drain action and exact-seek reorder publish decisions now live in `decode_loop_policy` with focused state tests.
- `DecodeThread`: codec send/receive SEH guards, return classification, and hardware device mutex wrapping now live in `codec_loop` with focused state tests.
- `DecodeThread`: frame conversion/publish failure handling and hardware visibility flush now live in `DecodedFramePublisher` with focused state tests.
- `DecodeThread`: drain-before-next-packet and EOF codec drain send/receive decisions now live in `decode_drain_policy` with focused state tests.
- `DecodeThread`: exact-seek reorder/pending candidate ownership and candidate memory counters now live in `ExactSeekCandidateStore` with focused state tests.
- `DecodeThread`: AVFrame timestamp rescale to microseconds now lives in `frame_timestamp_rescaler` with focused state tests.
- `DecodeThread`: exact-seek preview completion success gate, pause/drain state facts, and completion log counters now live in `exact_seek_publish_policy`.
- `DecodeThread`: exact-seek preview-after-collect publish gate and hardware exact-seek pacing gate moved into `decode_loop_policy`.
- `NativeResourceBudget`: track buffer queued-frame depth decision moved into `TrackBufferBudget` and is tested as a policy boundary.
- `AudioEngine::Impl`: track registry, buffer publication facts, and decode pause/seek fanout moved into `AudioTrackRegistry`.
- `AudioEngine::Impl`: nested FFmpeg audio decoder thread implementation moved into `AudioDecodeThread`.
- `AudioEngine::Impl`: nested waveOut output thread and WinMM device loop moved into `WaveOutOutput`.
- `AnalysisManager`: VAC2 session data, overlay chunk index/cache, decoded chunk LRU, and PTS-to-frame mapping moved into `AnalysisSession`.
- `AnalysisManager`: overlay track registration/snapshot storage moved into `AnalysisOverlayTrackRegistry`, backed by per-track `AnalysisSession` snapshots instead of recursive manager instances.
- Windows runner plugin: diagnostics, logging bootstrap, texture bridge, file picker, method dispatch, and MethodChannel diagnostics scope were split.
- Process-global logging/crash FFI ownership is now documented.

Still active:

- `Renderer` remains the coordination root.
- `Renderer` still owns public layout API/redraw invalidation and deferred seek execution.
- `DecodeThread` still owns drain-before-next-packet execution and decode-loop control flow.
- Target/feature boundaries are still too coupled.
- Packet queue capacity, analysis cache/file size, and runtime budget override policy are still distributed.

### `review_renderer.md`

Status: fixed for the current chat audit.

Fixed:

- Draw path now builds an immutable snapshot under `state_mutex_` before entering `device_mutex_`; `draw_frame()` no longer locks state or reads mutable tracks/layout directly.
- Render loop state transitions for preroll, paused preview, diagnostics, carry-forward, redraw, EOF, and frame deadlines now use explicit `state_mutex_` boundaries.
- `D3D11FramePresenter` slot resources are serialized internally, and prepared frames own SRV references plus UV scale snapshots through draw.
- Public renderer query APIs read tracks/duration under `state_mutex_`.
- Shutdown gates late demux/render callbacks, render-loop timer resolution is RAII scoped, the render thread has a `noexcept` exception boundary, and pending resize is dropped on loop exit.
- Borrowed backend child pointers were removed from `Renderer`; backend-owned helpers are now queried through `D3D11RenderBackend`.
- Renderer helper/policy files are grouped under `layout/`, `track/`, `seek/`, `render/`, `overlay/`, and `playback/` while `renderer.cpp/h` remain the facade/owner.

### `review_renderer_v2.md`

Status: accepted items fixed; stale findings documented.

Stale or already covered:

- `D3D11PreparedFrame` already owns SRV `ComPtr`s and carries `nv12_uv_scale_x/y` defaults of `1.0f`.
- `D3D11FramePresenter` slot resource access is already serialized internally across prepare/reset/move/memory stats. The suggested extra `device_mutex_` wrapper can still be evaluated, but the original "not thread-safe container" severity no longer matches current code.

Accepted:

- `PresentDecision` lacked track identity. A decision produced before remove/add/compact/recreate could later be applied to a reused slot, contaminating draw, geometry, cached decisions, carry-forward, seek-preview events, and stats.
- Non-headless present after draw failure, shutdown callback lifetime, render-loop crash state, and lifecycle long-lock follow-ups were valid and have been fixed.
- Seek-recreate stop/open work has been moved out of long `state_mutex_` critical sections; direct branch coverage still needs a future fault-injection/native integration seam.
- Focused native coverage now guards event-callback release, terminal render-loop state transition, and the swap-chain present skip decision after draw failure.

## Active Patch Queue

Next patch: resume broader renderer god-object ownership extraction now that the `review_renderer_v2.md` accepted safety items are closed.

Completed patch details through P123 are archived in `native/docs/NATIVE_STABILIZATION_HISTORY.md`.

### P124 - DecodeThread Frame Lifetime Boundary

Status: done in Patch 124.

Goal:

- Continue shrinking `DecodeThread` by extracting AVFrame ownership cleanup decisions around normal receive, exact-seek reorder collection, and failed publish paths.
- Keep FFmpeg frame allocation/freeing and hardware visibility waits in `DecodeThread`; move only deterministic unref/reuse intent into a focused policy/helper with tests.
- Preserve exact-seek preview behavior, conversion-failure handling, and buffer state transitions.

Validation:

- `python dev.py test --native-only`
- If touched code affects seek preview behavior, also run `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`.

Result:

- Added `AvFrameUnrefGuard` and `reset_reusable_av_frame` as the explicit reusable-frame lifetime boundary.
- Replaced scattered manual `av_frame_unref()` calls in normal receive, EOF drain, post-preview drain, exact-seek candidate handling, and seek reset with the focused guard/helper.
- Added native coverage for scope-exit unref, caller-owned dismiss, and null-safe reset.

### P125 - DecodeThread Publish Error Boundary

Status: done in Patch 125.

Goal:

- Remove the remaining exact-seek conversion-failure state writes from `DecodeThread::publish_exact_seek_window()`.
- Extend `DecodedFramePublisher` so exact-seek stable-frame reuse and converted-frame publish can share the same Error/pause/stop semantics as normal frame publish.
- Preserve exact-seek window ordering, hardware wait/flush behavior, stable snapshot reuse, and pending candidate movement.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `DecodedFramePublisher::push_converted_frame()` so already-converted/stable exact-seek frames share normal publish error handling.
- `convert_and_push_frame()` now delegates conversion result handling to the same helper.
- `DecodeThread::publish_exact_seek_window()` no longer writes conversion-failure Error/pause/running state directly.
- Added focused publisher coverage for prepared-frame publish and missing-frame error handling.

### P126 - DecodeThread Exact Seek Frame Publisher Boundary

Status: done in Patch 126.

Goal:

- Move the remaining exact-seek preview-window frame publish loop and pending-frame publish helper out of `DecodeThread`.
- Keep exact-seek candidate ownership in `ExactSeekCandidateStore`, publish-window sizing in `exact_seek_publish_policy`, and final post-preview state updates in `DecodeThread` for this patch.
- Preserve selected-frame hardware wait, subsequent-frame flush, stable snapshot reuse, pending tail movement, and logs.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `exact_seek_frame_publisher` for exact-seek preview-window frame publishing and pending candidate publishing.
- Moved selected-frame hardware wait, subsequent-frame flush, stable snapshot reuse, conversion failure cleanup, and pending tail movement out of `DecodeThread`.
- `DecodeThread` now keeps only publish scheduling, successful post-preview completion state, and log emission around exact-seek frame publishing.
- Added focused coverage for preview-window publish, pending publish, and conversion-failure candidate cleanup.

### P127 - FFI Lifecycle Shell Boundary

Status: done in Patch 127.

Goal:

- Continue reducing `ffi_exports.cpp` by moving player create/destroy/error/lifecycle command bodies behind a narrow lifecycle helper while keeping exported ABI functions as guarded shims.
- Keep `ffi_exports.cpp` responsible for `extern "C"` names and `ffi_guard`; move registry mutation and per-player error-copy bodies out.
- Preserve `naki_vr_player_create`, destroy/double-destroy, `last_error`, `player_get_error`, initialize v1/v2, and shutdown ABI behavior.

Validation:

- `python dev.py test --native-only`

Result:

- Added `ffi_player_lifecycle` for create/destroy/error copy, initialize v1/v2, and shutdown lifecycle command bodies.
- Reduced `ffi_exports.cpp` lifecycle functions to guarded ABI shims that call the lifecycle helper.
- Added focused lifecycle coverage for create/destroy/double-destroy, global/player error copy fallback, and null config validation.
- Kept existing C FFI ABI validation green.

### P128 - FFI Process-Global Shell Boundary

Status: done in Patch 128.

Goal:

- Move process-wide logging and crash-handler convenience command bodies out of `ffi_exports.cpp`.
- Keep the exported `naki_vr_configure_logging*`, `naki_vr_install_crash_handler*`, and `naki_vr_remove_crash_handler*` functions as guarded ABI shims.
- Preserve process-global semantics while making the global side effects explicit in a helper boundary.

Validation:

- `python dev.py test --native-only`

Result:

- Added `ffi_process_globals` for process-wide logging and crash-handler convenience command bodies.
- Reduced `ffi_exports.cpp` logging/crash functions to guarded ABI shims that call the process-global helper.
- Added focused coverage for null logging config validation and crash-handler install/remove command behavior.
- Kept existing C FFI ABI validation green.

### P129 - Renderer Playback Command Boundary

Status: done in Patch 129.

Goal:

- Continue shrinking `Renderer` by moving public playback command intent around play/pause/step fanout into a focused helper/policy.
- Keep `Renderer` responsible for lifecycle locks, playback clock ownership, and public API shape; move only deterministic command fanout/transition facts first.
- Preserve current play/pause/step behavior and existing UI playback regressions.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `renderer_playback_command_policy` for deterministic play/pause/step command plans.
- Replaced the public playback command preflight logic in `Renderer` with policy calls while keeping locks, playback clock commands, seek reset, and decode fanout execution inside `Renderer`.
- Added focused native coverage for play gating, pause intent, and step buffering/initialization gates.

### P130 - Renderer Seek Clock Boundary

Status: done in Patch 130.

Goal:

- Continue shrinking `Renderer::seek_internal` by extracting the global seek clock/deferred-gate decision facts into a focused helper/policy.
- Keep `Renderer` responsible for locks, playback clock mutation, pending seek-preview event state, callbacks, and per-track seek execution.
- Preserve exact/keyframe seek semantics and paused HEVC deferred seek behavior.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added a pure `RendererSeekClockGatePolicy` under `SeekCoordinator` for clock target and paused HEVC exact-seek defer-gate facts.
- Rewired `Renderer::seek_internal` to consume the plan before mutating playback clock or querying the coordinator.
- Added focused seek coordinator coverage for always-advance-clock behavior and paused HEVC exact-seek defer eligibility.

### P131 - Renderer Seek Logging Boundary

Status: done in Patch 131.

Goal:

- Continue shrinking `Renderer::seek_internal` by extracting seek log message data assembly and stable formatting decisions into a focused helper.
- Keep `Renderer` responsible for when logs are emitted, actual seek execution, callbacks, and state mutation.
- Preserve existing log text where possible so current diagnostics remain familiar.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `renderer_seek_log_policy` for seek request, clamp, per-track target clamp, HEVC coalescing, and cleared-track log facts.
- Rewired `Renderer::seek_internal` to consume log facts while preserving existing log text and emission points.
- Added focused native coverage for seek log fact assembly and per-track seek diagnostics gates.

### P132 - Renderer Layout Mutation Boundary

Status: done in Patch 132.

Goal:

- Continue shrinking `Renderer` by moving the remaining layout/frame-geometry mutation facts out of Renderer methods.
- Keep `Renderer` responsible for state locks, redraw invalidation, public API shape, and D3D presentation side effects.
- Preserve split/viewport/pan/zoom behavior and current geometry diagnostics.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/viewport_pan_layout_regression.csv`

Result:

- Added `adjust_layout_view_offset_for_resize` in `layout_geometry` for resize-driven view offset scaling.
- Rewired headless `Renderer::do_resize` to delegate display-size ratio and offset mutation to the layout helper while keeping locks and D3D resize in Renderer.
- Added focused layout geometry coverage for proportional offset scaling and invalid-size no-op behavior.

### P133 - DecodeThread Post-Preview Completion Boundary

Status: done in Patch 133.

Goal:

- Continue shrinking `DecodeThread` by extracting the exact-seek post-preview completion/drain scheduling facts that remain after `exact_seek_frame_publisher`.
- Keep `DecodeThread` responsible for AVFrame ownership, logging, buffer state writes, and loop control.
- Preserve paused preview publication, pending exact-seek candidate handling, and drain-before-next-packet behavior.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Extended `exact_seek_publish_policy` with a completion plan that gates failed preview publishes and carries pause/drain state facts plus selected/published/pending log counters.
- Rewired `DecodeThread::publish_exact_seek_window` to consume the completion plan while keeping TrackBuffer state writes, atomics, and logging emission inside `DecodeThread`.
- Added focused native coverage for successful, skipped, and conversion-failed completion plans.

### P134 - Native GodObject Round Archive

Status: done in Patch 134.

Goal:

- Move another early batch of completed GodObject patch details from this active cockpit into `native/docs/NATIVE_STABILIZATION_HISTORY.md`.
- Keep the active round focused on the current remaining GodObject risks and next patch queue.
- Preserve the current cross-check summary and latest active status.

Validation:

- `git diff --check`

Result:

- Archived completed patch details P116-P123 into `native/docs/NATIVE_STABILIZATION_HISTORY.md`.
- Kept the active cockpit focused on P124+ and the remaining GodObject risks.

### P135 - DecodeThread Exact Seek Publish Scheduling Boundary

Status: done in Patch 135.

Goal:

- Continue shrinking `DecodeThread` by extracting the remaining exact-seek publish scheduling decisions around preview-window readiness, EOF fallback publish, and hardware pacing.
- Keep `DecodeThread` responsible for AVFrame ownership, codec drain execution, perf counters, and final loop control.
- Preserve paused preview behavior and H.265 seek visual regression coverage.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added `decode_loop_policy` gates for exact-seek preview publish after candidate collection and hardware exact-seek pacing.
- Rewired the receive loop to use the policy gates while keeping candidate ownership, publish calls, sleeps, and loop breaks inside `DecodeThread`.
- Added focused native coverage for exact-seek preview publish and pacing gate combinations.

### P136 - Renderer Draw Snapshot Lock Boundary

Status: done in Patch 136.

Goal:

- Remove `draw_frame()`'s `state_mutex_` acquisition and direct mutable renderer-state reads from the D3D draw path.
- Build an immutable draw snapshot under `state_mutex_` before entering `device_mutex_`, including layout, background color, target size, geometry facts, active-track facts, and the `PresentDecision`.
- Preserve headless/non-headless presentation behavior, analysis overlay rendering, GPU metrics, and device-lost handling.
- Keep this patch focused on lock order and draw inputs; render-loop policy snapshots and presenter slot serialization follow in later patches.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/viewport_pan_layout_regression.csv ui_tests/analysis/spawn_h264.csv`

Result:

- Added `RendererDrawSnapshot` as the immutable state bundle consumed by D3D draw.
- `present_frame()`, `redraw_layout()`, and headless resize now build draw snapshots under `state_mutex_` before entering `device_mutex_`.
- `draw_frame()` no longer takes `state_mutex_` or directly reads `tracks_`, `layout_`, `background_color_`, or target dimensions.
- Analysis overlay draw now consumes the draw-track snapshot instead of reading `TrackPipelineManager` during D3D draw.

### P137 - Renderer Render Loop State Snapshot Boundary

Status: done in Patch 137.

Goal:

- Stop render-loop policy helpers from directly reading mutable `tracks_` outside a single state snapshot boundary.
- Move `last_decision_`, `preview_drawn_`, and `was_buffering_` render-loop transitions behind explicit `state_mutex_` sections or immutable per-tick facts.
- Preserve paused preview, preroll transition, carry-forward, EOF clamp, and frame-deadline sleep behavior.
- Keep presenter slot serialization for the following patch; this patch is about renderer-owned state reads/writes.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/h265_seek_visual_regression.csv ui_tests/timeline/h265_timeline_click_visual_regression.csv`

Result:

- Render-loop preroll, paused-preview, diagnostics, carry-forward, redraw, EOF, and frame-deadline decisions now read `tracks_`, `last_decision_`, `preview_drawn_`, and `was_buffering_` under explicit `state_mutex_` sections.
- `draw_paused_frame()` and step-forward last-frame commit no longer write `last_decision_` outside `state_mutex_`.
- Seek-preview event collection now snapshots track file IDs under `state_mutex_` before invoking callbacks outside renderer locks.

### P138 - Renderer Frame Presenter Serialization Boundary

Status: done in Patch 138.

Goal:

- Serialize `D3D11FramePresenter` slot resource access across `prepare_frame()`, `reset_track()`, `move_track()`, `reset_all()`, and `memory_stats()`.
- Prefer a focused internal presenter mutex for this patch so track lifecycle paths stop racing render draw without reshaping the full render-thread command model yet.
- Preserve existing slot compaction, seek reset, NV12 copy cache, software texture cache, and GPU memory diagnostics behavior.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv ui_tests/seek/h265_seek_visual_regression.csv`

Result:

- Added an internal `D3D11FramePresenter` mutex around slot resource mutation, preparation, reset/move, scale queries, and memory stats.
- `D3D11PreparedFrame` now keeps SRV `ComPtr` ownership and UV scale snapshots so prepared draw inputs stay alive even if a slot reset/move waits or runs before the draw returns.
- Renderer draw now uses the prepared-frame UV scale snapshot instead of re-querying presenter slot state during shader constant assembly.

### P139 - Renderer Query Lock Boundary

Status: done in Patch 139.

Goal:

- Add consistent `state_mutex_` coverage to `track_count()`, `duration_us()`, `has_track()`, `track_dimensions()`, and `track_infos()`.
- Keep these APIs as short read-only snapshots; do not widen NativePlayer outer locks in this patch.
- Preserve FFI query defaults and runner diagnostics payloads.

Validation:

- `python dev.py test --native-only`

Result:

- Public renderer query APIs now take `state_mutex_` while reading `tracks_` or cached duration state.
- Internal logs that already run under `state_mutex_` avoid re-entering those query helpers.
- Native-only regression passed after the query lock boundary change.

### P140 - Renderer Shutdown Callback And Loop Guard

Status: done in Patch 140.

Goal:

- Gate late demux/render callbacks after shutdown begins.
- Wrap render-loop timer resolution with RAII and add an exception boundary around the render thread.
- Drop pending resize state on render-loop exit instead of unconditionally flushing a resize during shutdown.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Coverage gap:

- Existing UI scripts cover software seek, shutdown/recreate, and HEVC hardware seek paths, but the attempted playing HEVC hardware timeline seek did not reliably trigger the `Recreating pipeline` branch. A direct fault-injection or native integration seam is still needed to assert the state lock is released during recreate stop/open/start.

Result:

- Renderer now sets a shutdown gate before stopping the render loop and resource teardown.
- Demux seek/error callbacks and headless frame callbacks check the shutdown gate before touching renderer-owned callback paths.
- Render loop work moved behind a `noexcept` entrypoint with RAII timer resolution and exception logging.
- Pending resize state is discarded on render-loop exit rather than applied during shutdown/recreate teardown.

### P141 - Renderer Backend Refs Cleanup

Status: done in Patch 141.

Goal:

- Remove unused or trivially replaceable borrowed backend raw pointers from `Renderer`.
- Prefer accessing D3D11 helpers through `D3D11RenderBackend` where it keeps ownership and shutdown ordering clearer.
- Keep behavior unchanged; this patch should only shrink dangling-pointer surface area.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Removed cached borrowed backend members for D3D device, frame presenter, headless output, shader/texture managers, and render resources.
- Renderer now queries backend-owned helpers through small accessors, keeping `D3D11RenderBackend` as the single owner of those objects.
- Resource teardown no longer has to manually null a parallel set of borrowed backend pointers.

### P142 - Renderer Directory Regroup

Status: done in Patch 142.

Goal:

- Move renderer helper/policy files into domain subdirectories after the concurrency fixes are stable.
- Keep `renderer.cpp/h` at the root as the facade/owner for now.
- Do not mix behavior changes with the file move; update includes and CMake/source lists only.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Result:

- Moved renderer helpers/policies into domain folders: `layout/`, `track/`, `seek/`, `render/`, `overlay/`, and `playback/`.
- Updated includes, CMake source lists, tests, runner references, tools, and docs for the new paths.
- Kept `renderer.cpp/h`, `renderer_config_validation.*`, `renderer_limits.h`, `audio_coordinator.*`, and `clock.*` at the renderer root.

### P143 - Renderer PresentDecision Identity Boundary

Status: done in Patch 143.

Goal:

- Attach track identity to every `PresentDecision` slot so decisions produced outside `state_mutex_` cannot be applied after remove/add/compact/recreate reuses the same slot.
- Filter draw snapshots, geometry updates, carry-forward, paused preview, step decisions, seek-preview events, and stats against current `file_id + generation`.
- Preserve legacy unit-test decisions that intentionally omit identity while production tracks receive nonzero generations.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Result:

- Added per-slot `file_id` and `track_generation` metadata to `PresentDecision` and `RenderSink`.
- Assigned monotonically increasing generations to track pipelines and propagated identity through add/remove/compact/recreate render-sink commits.
- Added shared present-decision identity helpers and used them before drawing, updating layout geometry, caching decisions, carrying frames forward, emitting seek-preview events, and collecting performance stats.
- Added native coverage proving RenderSink decisions carry and clear track identity.

### P144 - Renderer Non-Headless Present Failure Gate

Status: done in Patch 144.

Goal:

- In non-headless `present_frame()`, skip swap-chain present when `draw_frame()` fails.
- Preserve device-lost detection by checking `device_lost()` after a failed draw when a device exists.
- Keep headless publish behavior unchanged.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Coverage gap:

- Current tests compile and smoke the path, but do not fault-inject `draw_frame()` failure on a live swap chain.

Result:

- Non-headless `present_frame()` now only calls `device->present(0)` after a successful draw.
- Present publish metrics are recorded only for attempted swap-chain presents.

### P145 - Renderer Shutdown Event Callback Release

Status: done in Patch 145.

Goal:

- Clear `event_callback_` during shutdown/resource release so host callbacks do not survive across shutdown/reinitialize.
- Also clear callbacks when shutdown is called before renderer resources were initialized.
- Preserve the existing shutdown callback gate for late demux/render events.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Coverage gap:

- Current tests cover shutdown/recreate behavior, but do not directly introspect private callback storage.

Result:

- Added a focused `clear_event_callback()` helper guarded by `event_callback_mutex_`.
- `shutdown()` clears the callback on the no-resource early-return path.
- `release_resources_locked()` clears the callback before tearing down renderer-owned resources.

### P146 - Renderer Render-Loop Crash Terminal State

Status: done in Patch 146.

Goal:

- Make the render-loop exception boundary enter an explicit terminal runtime state instead of leaving `initialized_` true while the render thread is dead.
- Keep device-lost handling separate from non-D3D runtime exceptions.
- Preserve normal shutdown behavior, where resource release resets renderer state after the render thread joins.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv`

Coverage gap:

- Current tests cover the normal render loop and shutdown path, but do not fault-inject an exception inside `render_loop_body()`.

Result:

- Added `enter_terminal_render_loop_error_locked()` for non-D3D render-loop crashes.
- The render-loop `std::exception` and unknown-exception catches now mark `running_=false`, `playing_=false`, `initialized_=false`, pause playback/decode, and set `device_state_` to `Terminal`.

### P147 - Renderer Remove-Track Stop Outside State Lock

Status: done in Patch 147.

Goal:

- Shorten `remove_track()`'s `state_mutex_` critical section by detaching and compacting renderer state first, then stopping/joining the removed pipeline outside the state lock.
- Keep the public mutation serialized by `lifecycle_mutex_`.
- Preserve render-sink slot clearing, presenter reset/move, layout order removal, duration cache recompute, cached present-decision compaction, and playback resume policy.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Result:

- `remove_track()` now removes the slot from renderer-visible state under `state_mutex_`, stores the removed `TrackPipeline` in a local owner, and releases the state lock before stopping decode/demux.
- Removed demux callbacks are cleared before the detached pipeline is stopped outside the state lock.
- Existing compacted tracks remain committed to `RenderSink` with their `file_id + generation` identity before the removed pipeline is stopped.

### P148 - Renderer Add-Track Open/Start Outside State Lock

Status: done in Patch 148.

Goal:

- Shorten `add_track()`'s `state_mutex_` critical section by moving pipeline open/probe, decoder construction, callback wiring, and demux start outside the state lock.
- Keep the public mutation serialized by `lifecycle_mutex_`.
- Preserve slot reservation, playback pause/rollback, file-id/generation assignment, layout append, duration cache update, render-sink commit, presenter reset, current-clock seek alignment, and cached-decision invalidation.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv`

Result:

- `add_track()` now uses short state-lock sections for preflight/playback pause and final commit, while pipeline creation/start runs without holding `state_mutex_`.
- Failed pipeline creation/start rolls back the temporary playback pause under the state lock.

### P149 - Renderer Seek Recreate Outside State Lock

Status: done in Patch 149.

Goal:

- Shorten seek-triggered pipeline recreate by detaching renderer-visible slot state under `state_mutex_`, then stopping the old pipeline and opening/starting the replacement outside that lock.
- Keep seek/recreate serialized with public lifecycle mutations while avoiding render/query starvation during stop/open/start.
- Preserve render-sink clearing, presenter reset, track identity generation, initial seek, paused HEVC recreate tagging, and cached-decision invalidation.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

Result:

- `seek_internal()` now receives an owning `std::unique_lock` so the recreate path can deliberately drop and reacquire `state_mutex_`.
- HEVC recreate detaches the old slot state, clears stale present-decision data, stops decode/demux, waits for D3D11VA teardown, creates the replacement pipeline, and starts it without holding `state_mutex_`.
- The final replacement commit is guarded by `state_mutex_`, using a fresh track generation so old frame decisions cannot be reused for the recreated slot.

## Do-Not-Drift List

- Do not let runner plugin cosmetics displace the remaining `review_godobject.md` owner-boundary work.
- Keep overlay regression coverage in place before starting large Renderer ownership splits.
- Do not combine renderer directory moves with concurrency or draw-path behavior fixes.
- Do not add broad fallback image conversion libraries; pixel-format support must stay deterministic.
- Do not batch unrelated cleanup with behavior fixes.
- Do not mark a chat item fixed without a test or an explicit documented coverage gap.
