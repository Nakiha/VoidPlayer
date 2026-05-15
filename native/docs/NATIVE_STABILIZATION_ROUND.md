# Native Stabilization Round

本文件是 native 深度清理的当前驾驶舱。旧的 patch 流水账已归档到
`native/docs/NATIVE_STABILIZATION_HISTORY.md`，避免后续继续被 runner bridge cleanup 带偏。

## Sources

- `build/chat/review_native.md`
- `build/chat/review_overlay.md`
- `build/chat/review_godobject.md`
- `build/chat/split_adv.md`
- `native/docs/NATIVE_REFACTOR_TODO.md`

## Focus Rule

当前优先级：

1. `review_godobject.md` 中 Renderer owner boundary 的下一刀。
2. `ffi_exports.cpp` / `TrackPipelineManager` / `DecodeThread` 这些二级 God Module 的收缩。
3. `review_native.md` / `review_overlay.md` 已修 correctness 回归防线不能倒退。

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
- `DecodeThread`: exact-seek lookbehind, preview-window readiness, and preview-frame selection now live in `exact_seek_window` with focused state tests.
- `DecodeThread`: pending exact-seek publish, drain-before-next-packet, paused consumption, and stale-packet discard guards now live in `decode_loop_policy` with focused state tests.
- `DecodeThread`: EOF drain action and exact-seek reorder publish decisions now live in `decode_loop_policy` with focused state tests.
- `DecodeThread`: codec send/receive SEH guards, return classification, and hardware device mutex wrapping now live in `codec_loop` with focused state tests.
- `DecodeThread`: frame conversion/publish failure handling and hardware visibility flush now live in `DecodedFramePublisher` with focused state tests.
- `DecodeThread`: drain-before-next-packet and EOF codec drain send/receive decisions now live in `decode_drain_policy` with focused state tests.
- `DecodeThread`: exact-seek reorder/pending candidate ownership and candidate memory counters now live in `ExactSeekCandidateStore` with focused state tests.
- `DecodeThread`: AVFrame timestamp rescale to microseconds now lives in `frame_timestamp_rescaler` with focused state tests.
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
- `Renderer` still owns layout mutation and deferred seek execution.
- `DecodeThread` still owns exact-seek publish scheduling and post-preview completion wiring.
- Target/feature boundaries are still too coupled.
- Packet queue capacity, analysis cache/file size, and runtime budget override policy are still distributed.

## Active Patch Queue

Next patch: P132 Renderer Layout Mutation Boundary.

Completed patch details through P115 are archived in `native/docs/NATIVE_STABILIZATION_HISTORY.md`.

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

Goal:

- Continue shrinking `Renderer` by moving the remaining layout/frame-geometry mutation facts out of Renderer methods.
- Keep `Renderer` responsible for state locks, redraw invalidation, public API shape, and D3D presentation side effects.
- Preserve split/viewport/pan/zoom behavior and current geometry diagnostics.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/viewport_pan_layout_regression.csv`

## Do-Not-Drift List

- Do not let runner plugin cosmetics displace the remaining `review_godobject.md` owner-boundary work.
- Keep overlay regression coverage in place before starting large Renderer ownership splits.
- Do not add broad fallback image conversion libraries; pixel-format support must stay deterministic.
- Do not batch unrelated cleanup with behavior fixes.
- Do not mark a chat item fixed without a test or an explicit documented coverage gap.
