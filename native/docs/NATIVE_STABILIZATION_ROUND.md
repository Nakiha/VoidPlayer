# Native Stabilization Round

本文件记录 native 深度清理收敛轮的范围、证据、进度和验证结果。

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
| `FrameCaptureService` | 边界清楚，能顺带修 capture 锁粒度 | 可作为前五项后的第一拆 |
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

- Second-tier native correctness backlog from `review_native.md` is now complete except the broader #6/#7 facade/FFI lifecycle cleanup items.

## Final Cross-Check

完成本轮后，逐条回看 chat 文件，更新下列结果：

| 来源 | 复核项 | 结果 |
| --- | --- | --- |
| `review_native.md` | 13 条 native correctness / lifecycle / validation 问题 | fixed: #1/#2/#3/#4/#5/#8/#9/#10/#11/#12/#13; accepted-backlog: #6/#7 |
| `review_godobject.md` | God Object 排名和 owner boundary 判断 | fixed: AudioMixer boundary + Analysis session snapshot; accepted-backlog: remaining owner splits |
| `review_overlay.md` | AnalysisManager、VACHUNK、overlay cache、D3D pass 风险 | fixed: AnalysisManager session + current-base chunk filter; accepted-backlog: remaining overlay/cache/render-pass items |
| `split_adv.md` | Patch 顺序和“不贪大”边界 | fixed: Patch 1-12 executed in stabilization-sized slices |

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
- #8 layout validation: Patch 7 tightened split/zoom/order checks using file-ID order semantics.
- #9 RGBA texture size validation: Patch 8 added texture dimension/stride guardrails.
- #10 demux read error propagation: Patch 9 emits track error events and marks the track buffer Error on non-EOF read errors.
- #11 `avcodec_open2()` SEH guard: Patch 10 routes codec open and software fallback open through a noinline SEH wrapper.
- #12 odd-dimension software path: Patch 11 pads coded CPU NV12/P010 buffers while preserving odd display dimensions.
- #13 D3D shutdown `ClearState + Flush`: Patch 12 clears immediate-context bindings and flushes before device release.

accepted-backlog:

- #6 NativePlayer facade locking remains a lifecycle boundary cleanup.
- #7 FFI long-operation serialization remains an ABI/registry cleanup.

not-applicable:

- None.

### `review_godobject.md`

fixed:

- `AudioEngine::Impl`: Patch 2 extracted `AudioMixer`, separating output submission from mixer/PCM consumption policy.
- `AnalysisManager`: Patch 5 introduced session snapshots and moved overlay chunk cache/index state into the session.

accepted-backlog:

- `Renderer` remains the root coordination object; future work should move one owner boundary at a time.
- `windows/runner/video_renderer_plugin.cpp` remains a bridge God Module; split dispatcher / texture bridge / diagnostics / capture later.
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

accepted-backlog:

- Remaining second-priority owner boundary work should continue as explicit owner-boundary slices such as `FrameCaptureService`; avoid jumping straight into a large Renderer split.

not-applicable:

- None.
