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
| S3 | `RenderSink` 长期保存裸 `TrackBuffer*` | `RenderSink::tracks_` 是裸指针数组；render loop `evaluate()` 与 remove/compact 不共享明确锁契约 | add/remove/compact 时 UAF 或错轨 | TODO |
| S4 | Headless shared texture 没有 in-flight tracking | `pick_free_buffer()` 固定返回 `(front + 2) % 3`；release callback 只保证 lifetime，不保证内容不被重写 | Flutter 仍采样旧 texture 时 native 覆盖导致闪帧/撕裂 | TODO |
| S5 | `AnalysisManager` session/global state 并发风险 | `loaded_ / vac2_base_ / analysis_path_` 无 session 级锁或 immutable snapshot；render thread 可同时读 overlay frame | render/FFI/load/unload 并发 UB 或错 chunk | TODO |

第二梯队，确认存在但本轮可以排在前五项之后：

| ID | 问题 | 当前证据 | 建议时机 |
| --- | --- | --- | --- |
| S6 | `capture_front_buffer_locked()` 持 texture mutex 做 GPU copy/map | `Renderer::capture_front_buffer()` 同时持 `device_mutex_` 和 texture mutex 调 staging copy/map | 可跟 `FrameCaptureService` 一起修 |
| S7 | layout validation 太宽松 | `validate_layout_state()` 只检查 enum、finite、zoom positive | `LayoutController` 或小防线 patch |
| S8 | `TextureManager::create_rgba_texture()` 缺尺寸校验 | RGBA create 直接 cast width/height，其他 create API 有基本校验 | 小防线 patch |
| S9 | demux read error 没传播成明确 track error/event | `DemuxThread::run()` 非 EOF read error 后 break，最后只 `abort_outputs()` | error model patch |
| S10 | `avcodec_open2()` 未包 SEH guard | send/receive 已有 SEH wrapper，open 阶段仍直调 | decode hardening patch |
| S11 | odd-dimension software path 直接拒绝 | `calculate_yuv420_layout()` 要求 width/height 都是偶数 | compatibility patch |
| S12 | `D3D11Device::shutdown()` 缺 `ClearState + Flush` | shutdown 直接 reset swapchain/context/device | cleanup patch |

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

## Final Cross-Check

完成本轮后，逐条回看 chat 文件，更新下列结果：

| 来源 | 复核项 | 结果 |
| --- | --- | --- |
| `review_native.md` | 13 条 native correctness / lifecycle / validation 问题 | TODO |
| `review_godobject.md` | God Object 排名和 owner boundary 判断 | TODO |
| `review_overlay.md` | AnalysisManager、VACHUNK、overlay cache、D3D pass 风险 | TODO |
| `split_adv.md` | Patch 顺序和“不贪大”边界 | TODO |

复核时只标三类状态：

- `fixed`: 本轮已修且有验证。
- `accepted-backlog`: 仍存在，但明确进入后续 backlog。
- `not-applicable`: 源码已变化或 chat 判断不再成立，并写明证据。
