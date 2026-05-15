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
- `DecodeThread`: exact-seek lookbehind, preview-window readiness, and preview-frame selection now live in `exact_seek_window` with focused state tests.
- `DecodeThread`: pending exact-seek publish, drain-before-next-packet, paused consumption, and stale-packet discard guards now live in `decode_loop_policy` with focused state tests.
- `DecodeThread`: EOF drain action and exact-seek reorder publish decisions now live in `decode_loop_policy` with focused state tests.
- `NativeResourceBudget`: track buffer queued-frame depth decision moved into `TrackBufferBudget` and is tested as a policy boundary.
- `AudioEngine::Impl`: track registry, buffer publication facts, and decode pause/seek fanout moved into `AudioTrackRegistry`.
- `AudioEngine::Impl`: nested FFmpeg audio decoder thread implementation moved into `AudioDecodeThread`.
- `AudioEngine::Impl`: nested waveOut output thread and WinMM device loop moved into `WaveOutOutput`.
- Windows runner plugin: diagnostics, logging bootstrap, texture bridge, file picker, method dispatch, and MethodChannel diagnostics scope were split.
- Process-global logging/crash FFI ownership is now documented.

Still active:

- `Renderer` remains the coordination root.
- `ffi_exports.cpp` still carries lifecycle create/destroy/error APIs and process-global logging/crash convenience shells.
- `Renderer` still owns layout mutation, public playback commands, global seek clock/deferred gates, and seek logging.
- `DecodeThread` main loop still owns codec send/receive, EOF drain, AVFrame ownership, and hardware visibility transitions.
- Target/feature boundaries are still too coupled.
- Packet queue capacity, analysis cache/file size, and runtime budget override policy are still distributed.

## Active Patch Queue

Next patch: P110 AnalysisManager Session Boundary.

Completed patch details through P99 are archived in `native/docs/NATIVE_STABILIZATION_HISTORY.md`.

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

Goal:

- Resume `review_godobject.md`'s `AnalysisManager` finding by extracting the next session/registry/cache-facing boundary that still lives in the singleton manager.
- Preserve existing analysis FFI/session snapshot behavior and overlay cache correctness fixed in the overlay rounds.
- Choose the exact patch boundary after re-reading current `analysis_manager.*`, because several earlier overlay fixes already moved part of the state model.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv`

## Do-Not-Drift List

- Do not let runner plugin cosmetics displace the remaining `review_godobject.md` owner-boundary work.
- Keep overlay regression coverage in place before starting large Renderer ownership splits.
- Do not add broad fallback image conversion libraries; pixel-format support must stay deterministic.
- Do not batch unrelated cleanup with behavior fixes.
- Do not mark a chat item fixed without a test or an explicit documented coverage gap.
