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

1. `review_overlay.md` 里仍会线上咬人的 overlay/cache/IO/D3D 状态问题。
2. `review_native.md` 的 correctness / race / lifetime 回归防线。
3. `review_godobject.md` 中 native owner boundary 的下一刀。

暂时不要继续做纯 runner plugin 清理，除非它直接关闭 native/overlay 的 process-global、测试隔离或上屏正确性问题。

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
- Windows runner plugin: diagnostics, logging bootstrap, texture bridge, file picker, method dispatch, and MethodChannel diagnostics scope were split.
- Process-global logging/crash FFI ownership is now documented.

Still active:

- `Renderer` remains the coordination root.
- `ffi_exports.cpp` remains an ABI God Module candidate.
- `TrackPipelineManager` still hides lifecycle factory order and should keep shrinking.
- `DecodeThread` remains a large seek/decode state machine.
- Target/feature boundaries are still too coupled.
- Resource budget policy is still distributed.

## Active Patch Queue

Next patch: return to `review_godobject.md` owner-boundary cleanup.

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

## Later Native Owner-Boundary Queue

These are real, but lower priority than the overlay backlog above:

- Split `AnalysisOverlayRenderer` from `Renderer`.
- Continue `LayoutController` ownership after the first order-controller slice.
- Extract `RenderLoopController` only after state/capture/overlay boundaries are calmer.
- Add `DeviceLossPolicy`.
- Split `ffi_exports.cpp` into ABI shim / registry / commands / marshalling.
- Add `DecodeThread` seek/drain/flush state-machine tests.
- Continue `TrackPipelineManager` factory/lifecycle split.
- Design target boundaries and feature options.
- Centralize queued-frame, exact-seek, analysis-cache, and runtime budget policy.

## Do-Not-Drift List

- Do not let runner plugin cosmetics displace the remaining `review_godobject.md` owner-boundary work.
- Keep overlay regression coverage in place before starting large Renderer ownership splits.
- Do not add broad fallback image conversion libraries; pixel-format support must stay deterministic.
- Do not batch unrelated cleanup with behavior fixes.
- Do not mark a chat item fixed without a test or an explicit documented coverage gap.
