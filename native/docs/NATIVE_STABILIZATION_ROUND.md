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

Status: partially fixed, still the main active backlog.

Fixed:

- AnalysisManager lifecycle data race: session snapshots replace mutable singleton session reads.
- Overlay chunk consistency: chunk index filters by codec, base content revision, track index, and required CU geometry features.
- VACache publication: cache files are published through atomic replace without deleting the final path first, and tmp names are unique across path/PID/TID/counter/time tokens.

Active backlog:

- VACHUNK hot-path IO/memory amplification: reading one frame can decode/copy whole chunk sections repeatedly.
- `overlay_raster.cpp` helper hardening: aliasing, zero-size, and size-multiply overflow guards.
- D3D overlay pass state contract: mask/color/invert pass state ownership and SRV/RTV hazard rules need to be explicit.
- High-resolution and tiny-rect overlay precision tests for 16-bit packed rects.
- Overlay generation budget should honor current-hash remaining budget, not only raw max cache bytes.
- VACHUNK checksum semantics are not enforced.
- record count truncation should be guarded before write validation.
- opacity 0 semantics need to match UI expectations.
- VAC2 frame model still has packet-index assumptions that can cause future overlay alignment issues.

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

Next patch: P31 VACHUNK Hot-Path Cache.

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
- analysis UI smoke if render-path behavior changes.

### P32 - Overlay Raster Hardening

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

### P33 - D3D Overlay Pass Contract

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
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv`

### P34 - Overlay Precision And Semantics Tests

Source: `review_overlay.md`.

Goal:

- Add regression coverage for tiny rects, edge rects, shared boundaries, 4K/8K-style coordinate ranges, grid-only paths, and opacity 0.
- Fix behavior only where tests expose incorrect semantics.

Validation:

- `python dev.py test --native-only`
- targeted analysis UI scripts when visible output semantics change.

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

- Do not prioritize runner plugin cosmetics while `review_overlay.md` P1 items remain open.
- Do not start a large Renderer split before P31-P33 are handled.
- Do not add broad fallback image conversion libraries; pixel-format support must stay deterministic.
- Do not batch unrelated cleanup with behavior fixes.
- Do not mark a chat item fixed without a test or an explicit documented coverage gap.
