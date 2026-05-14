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
- `Renderer`: render-loop debounce, diagnostics cadence, and frame-deadline sleep policy moved into `RenderLoopController`.
- `Renderer`: remove-track stop/compact render-sink/presenter slot side effects and cached `PresentDecision` frame compaction moved into `track_lifecycle`.
- `Renderer`: add-track current-clock seek target clamp, buffer/queue flush, audio pause, and seek type choice moved into `track_lifecycle`.
- `Renderer`: HEVC hardware seek recreate/coalesce/error decision moved into `SeekCoordinator` policy.
- `Renderer`: generic per-track seek preparation and post-recreate seek submission moved into `track_lifecycle`.
- `ffi_exports.cpp`: player handle registry, gate lease, thread-local last error, and per-player error state moved into `ffi_player_registry`.
- `ffi_exports.cpp`: ABI/config/log/layout/seek enum marshalling moved into `ffi_marshalling` with focused tests, leaving exported functions thinner.
- `ffi_exports.cpp`: playback/query/track/layout command bodies moved into `ffi_player_commands` with focused command tests.
- `TrackPipelineManager`: demux/decode pipeline construction moved into `TrackPipelineFactory`, leaving manager focused on slot storage, stop, and compact.
- `Renderer`: track pipeline metadata setup, callback/audio hook registration, demux start, and failed-start rollback moved into `track_lifecycle`.
- `DecodeThread`: exact-seek lookbehind, preview-window readiness, and preview-frame selection now live in `exact_seek_window` with focused state tests.
- `DecodeThread`: pending exact-seek publish, drain-before-next-packet, paused consumption, and stale-packet discard guards now live in `decode_loop_policy` with focused state tests.
- `DecodeThread`: EOF drain action and exact-seek reorder publish decisions now live in `decode_loop_policy` with focused state tests.
- `NativeResourceBudget`: track buffer queued-frame depth decision moved into `TrackBufferBudget` and is tested as a policy boundary.
- Windows runner plugin: diagnostics, logging bootstrap, texture bridge, file picker, method dispatch, and MethodChannel diagnostics scope were split.
- Process-global logging/crash FFI ownership is now documented.

Still active:

- `Renderer` remains the coordination root.
- `ffi_exports.cpp` still carries lifecycle create/destroy/error APIs and process-global logging/crash convenience shells.
- `Renderer` still owns track slot commit, layout mutation, playback pause/resume, global seek clock/deferred gates, and seek pipeline recreation.
- `DecodeThread` main loop still owns codec send/receive, EOF drain, AVFrame ownership, and hardware visibility transitions.
- Target/feature boundaries are still too coupled.
- Packet queue capacity, analysis cache/file size, and runtime budget override policy are still distributed.

## Active Patch Queue

Next patch: P55 Renderer Seek Pipeline Recreate Boundary.

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

Goal:

- Move `recreate_pipeline_for_seek` stop/recreate/start/render-sink commit choreography into a lifecycle helper.
- Keep the HEVC recreate decision, pipeline factory callback, and Renderer-owned audio/render-sink hooks at the Renderer boundary.

Validation:

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/seek/shutdown_during_seek_recreate_smoke.csv`

## Do-Not-Drift List

- Do not let runner plugin cosmetics displace the remaining `review_godobject.md` owner-boundary work.
- Keep overlay regression coverage in place before starting large Renderer ownership splits.
- Do not add broad fallback image conversion libraries; pixel-format support must stay deterministic.
- Do not batch unrelated cleanup with behavior fixes.
- Do not mark a chat item fixed without a test or an explicit documented coverage gap.
