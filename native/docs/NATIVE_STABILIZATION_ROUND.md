# Native Stabilization Round

本文件是 native 深度清理的当前驾驶舱。旧的 patch 流水账已归档到
`native/docs/NATIVE_STABILIZATION_HISTORY.md`，避免后续继续被 runner bridge cleanup 带偏。

## Sources

- `build/chat/review_native.md`
- `build/chat/review_overlay.md`
- `build/chat/review_godobject.md`
- `build/chat/review_renderer.md`
- `build/chat/split_adv.md`
- `native/docs/NATIVE_REFACTOR_TODO.md`

## Focus Rule

当前优先级：

1. `review_renderer.md` 点名的 Renderer 并发状态模型：draw snapshot 锁顺序、render-loop state snapshot、presenter slot serialization、query API locks、shutdown callback gate / render-loop RAII。
2. `review_godobject.md` 中 Renderer owner boundary 的后续拆分，但必须先守住状态访问和锁契约。
3. `ffi_exports.cpp` / `TrackPipelineManager` / `DecodeThread` 这些二级 God Module 的收缩，排在 Renderer 并发收敛之后。
4. `review_native.md` / `review_overlay.md` 已修 correctness 回归防线不能倒退。

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
- `Renderer` draw/present path still violates the documented lock order by entering D3D draw under `device_mutex_` and later taking `state_mutex_`.
- `Renderer` render loop still reads/writes `tracks_`, `last_decision_`, `preview_drawn_`, and `was_buffering_` without a single consistent state snapshot boundary.
- `D3D11FramePresenter` slot resources are mutated by track lifecycle paths while render draw can prepare frames.
- `Renderer` public query APIs still have inconsistent `state_mutex_` coverage.
- Renderer shutdown still needs a late-callback gate plus a render-loop exception/timer guard.
- `Renderer` still owns public layout API/redraw invalidation and deferred seek execution.
- `DecodeThread` still owns drain-before-next-packet execution and decode-loop control flow.
- Target/feature boundaries are still too coupled.
- Packet queue capacity, analysis cache/file size, and runtime budget override policy are still distributed.

### `review_renderer.md`

Status: accepted, high priority. This is not a stale or mostly-wrong chat audit.

Verified:

- Lock-order inversion exists: `present_frame()` / `redraw_layout()` take `device_mutex_` and then enter `draw_frame()`, while `draw_frame()` later takes `state_mutex_`; `gpu_memory_stats()` follows the documented `state_mutex_ -> device_mutex_` order, so a real deadlock path exists.
- `draw_frame()` still reads `tracks_` outside `state_mutex_`, including layout geometry, color defaults, frame preparation gates, and analysis overlay draw inputs.
- `render_loop()` still calls policy helpers directly on `tracks_` and mutates `last_decision_`, `preview_drawn_`, and `was_buffering_` outside one consistent state boundary.
- `D3D11FramePresenter` has no internal mutex; `prepare_frame()`, `reset_track()`, `move_track()`, and `memory_stats()` all touch the same slot resources.
- `track_count()`, `duration_us()`, `has_track()`, `track_dimensions()`, and `track_infos()` do not consistently take `state_mutex_`; NativePlayer's outer shared lock does not serialize query vs mutation.
- `render_loop()` still uses manual `timeBeginPeriod()` / `timeEndPeriod()` and flushes pending resize after the loop exits; shutdown callback gating remains soft.
- The flat `native/video_renderer/` root is no longer a good long-term shape for renderer policy files, but directory regrouping should be a separate mechanical patch after the concurrency fixes.

## Active Patch Queue

Next patch: P138 Renderer Frame Presenter Serialization Boundary.

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

Goal:

- Serialize `D3D11FramePresenter` slot resource access across `prepare_frame()`, `reset_track()`, `move_track()`, `reset_all()`, and `memory_stats()`.
- Prefer a focused internal presenter mutex for this patch so track lifecycle paths stop racing render draw without reshaping the full render-thread command model yet.
- Preserve existing slot compaction, seek reset, NV12 copy cache, software texture cache, and GPU memory diagnostics behavior.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/track/remove_middle_compact_regression.csv ui_tests/seek/h265_seek_visual_regression.csv`

## Do-Not-Drift List

- Do not let runner plugin cosmetics displace the remaining `review_godobject.md` owner-boundary work.
- Keep overlay regression coverage in place before starting large Renderer ownership splits.
- Do not combine renderer directory moves with concurrency or draw-path behavior fixes.
- Do not add broad fallback image conversion libraries; pixel-format support must stay deterministic.
- Do not batch unrelated cleanup with behavior fixes.
- Do not mark a chat item fixed without a test or an explicit documented coverage gap.
