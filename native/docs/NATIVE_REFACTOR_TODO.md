# Native Refactor Todo

本文件是 native 层硬化的主动追踪表。来源是 `build/gpt_native_adv.md` 的静态审查，以及对当前代码树的快速核验。

当前收敛轮的逐 patch 账本见 [NATIVE_STABILIZATION_ROUND.md](NATIVE_STABILIZATION_ROUND.md)。该文件用于记录 `build/chat/*.md` 核验结果、每个 stabilization patch 的边界、验证命令和最终复核结论。

使用规则：

- 每一轮只选一个清晰边界，不顺手堆无关改动。
- 开工前先确认本文件对应问题仍存在；完成后更新状态、证据和验证结果。
- 修改 native C++ 后至少跑 `python dev.py test --native-only`。
- 影响 Flutter runner、Texture、上屏、窗口交互或 `windows/runner/` 时，按 `AGENTS.md` 补跑带 `--build` 的相关 UI 脚本。
- 新增 ABI、release、CI、packaging 规则时，同步更新 `native/docs/BUILD_AND_TEST.md`、`native/docs/FFI_AND_BINDINGS.md` 或顶层发布说明。

## GPT Review 核验结论

总体判断：`build/gpt_native_adv.md` 大方向属实，但有些点已经比 review 描述更靠前。它最有价值的部分不是指出“代码烂”，而是指出 native 边界管理已经追不上复杂度：license/release、ABI、Renderer、runner plugin、CI、全局状态、资源预算需要分批硬化。

已核验为属实或基本属实：

- FFmpeg runtime 合规仍需要 release 级闭环。当前顶层 `LICENSE` 是 GPL，`lib/app_metadata.dart` 标注 GPLv3/FFmpeg，`native/THIRD_PARTY_NATIVE.md` 和 staging 规则会复制 FFmpeg `README.txt` / `LICENSE*`；但还缺面向 release artifact 的 NOTICE/source-offer/configure-flags 检查。
- `Renderer` 仍是大协调器。`native/video_renderer/renderer.h` 同时拥有 playback、track lifecycle、seek、layout、D3D11 backend、texture sharing、capture、analysis overlay、device-lost、render thread、metrics 和锁。
- Windows runner plugin 仍过大。`windows/runner/video_renderer_plugin.cpp` 约 1415 行，仍混合 MethodChannel handlers、player lifecycle、event bridge 和 process-global FFI diagnostics。
- C FFI ABI v1 仍偏窄。`naki_vr_player_config_t.video_paths` 是 null-terminated `const char**`，很多 mutating API 仍返回 `void`，`last_error` 仍是 thread-local，`player` 参数暂未提供 per-player 错误状态。
- config validation 仍分散。FFI 只校验 ABI/log/dimension/layout/speed 的一部分，MethodChannel 有自己的 dimension/speed/layout 检查，FrameConverter 又有独立的 `kMaxDecodedDimension` / `kMaxCpuFrameBytes`。
- `NativePlayer::initialize()` 的生命周期顺序有副作用风险：先 `playback_.start_session()`，再调用 `renderer_.initialize(config)`；重复 initialize 会先触碰 playback/audio session，再被 Renderer 拒绝。
- UTF-8 path helper 仍未严格校验非法 UTF-8。`native/common/win_utf8.h` 使用 `MultiByteToWideChar(CP_UTF8, 0, ...)`，没有 `MB_ERR_INVALID_CHARS`。
- resource budget 仍不够集中。单帧 CPU buffer 保险丝存在，但最大轨道数、路径数量、queued frames、exact seek reorder、analysis cache/file size 等预算没有统一入口。
- CI 仍不是“开源 native 项目级硬”。已有 Windows native test、Release matrix build、FFI smoke；但缺 Debug matrix、clang-cl、static analysis、sanitizer/替代检查、clean dist load smoke、release compliance smoke 和 CMakePresets。
- target/feature 边界仍重。`video_renderer_lib` 链接 `analysis_lib`、FFmpeg、D3D11、DXGI、d3dcompiler、winmm，analysis overlay 不是可选 feature。

已部分修复，review 中较旧的表述不再完全准确：

- Renderer FFI 已不再是裸 `NativePlayer*` live-set；当前使用 `shared_ptr` registry、per-handle mutex、destroy 先 unregister 后 shutdown。
- logging/crash 已从 Renderer 生命周期中抽出，runner 显式 opt-in；但 FFI 仍有 process-global `naki_vr_configure_logging()` / crash handler convenience API，需要标注 API 稳定性。
- analysis FFI 已有 handle registry、`shared_ptr` pinning、size/version v2 struct、last error；但 legacy singleton/global API 和全局 PTS callback 仍存在。
- 依赖锁定和 third-party manifest 已经改善；剩余重点是 release artifact 合规和 CI 检查，而不是“完全没有依赖清单”。
- parser/queue 已有 property-style 覆盖；剩余高风险在 decode/seek/render/shutdown/device/thread 交叉区。

## P0 - Release / License Compliance

Status: done in Round 13.

目标：让分发产物对 GPL FFmpeg runtime 的义务不可遗漏。

证据：

- `native/THIRD_PARTY_NATIVE.md` 明确默认 `windows/libs/ffmpeg` 是 gyan.dev FFmpeg 8.1 full shared GPL v3 package。
- `native/docs/BUILD_AND_TEST.md` 说明 runtime copy 会带 `README.txt` 和 `LICENSE*`。
- 顶层只有 `LICENSE`，当前未看到独立 `NOTICE` 或 release artifact compliance smoke。

TODO:

- [x] 新增顶层 `THIRD_PARTY_NOTICES.md`，集中列出 FFmpeg runtime、zstd、spdlog、Catch2、FFmpeg analyzer、Flutter/third_party desktop_drop 的分发说明。
- [x] 明确 release 包必须包含：顶层 GPL license、FFmpeg package license、FFmpeg README/source commit/configure flags、native third-party manifest。
- [x] 在 release/package 脚本中加入 machine-checkable compliance smoke，检查 staged package 存在上述文件。
- [x] 在 `native/docs/BUILD_AND_TEST.md` 增加“发布包合规检查”章节。
- [x] CI 增加 source-tree release compliance notice check；artifact/stage check 由 `python dev.py package` 执行。

建议验证：

- `python scripts/dev/check_release_compliance.py` passed on 2026-05-10.

## P0 - C FFI ABI v2 / Error Model

Status: done in Round 12.

目标：让 C ABI 更适合 Dart/Rust/Go/Python 绑定，减少未定义扫描、跨线程错误丢失和未来破 ABI 风险。

证据：

- `naki_vr_player_config_t.video_paths` 是 null-terminated `const char**`。
- `play/pause/seek/shutdown/remove_track/apply_layout/configure_logging/install_crash_handler` 等仍返回 `void`。
- `naki_vr_last_error(player, ...)` 当前忽略 `player`，使用 thread-local last-error。

TODO:

- [x] 保留 ABI v1，新增 ABI v2 config：`const char** video_paths` + `size_t video_path_count`。
- [x] 为 v2 mutating APIs 返回 `naki_vr_status_t`；旧 `void` API 作为兼容 wrapper。
- [x] 设计 per-player error slot，保留 thread-local last-error 作为补充诊断。
- [x] 增加 `naki_vr_player_get_error(player, ...)` per-player query，并保持 destroy 后旧 token 走 thread-local invalid-handle error。
- [x] 为 path count、空路径、过长路径、最大轨道数加 ABI 层校验；重复路径暂不禁止，因多轨同源文件在调试和对比场景下仍可用。
- [x] 将 FFI ABI/config/log/layout/seek enum marshalling 从 `ffi_exports.cpp` 拆到 `ffi_marshalling`，并增加 focused native 单测。
- [x] 将 FFI playback/query/track/layout checked-player command bodies 从 `ffi_exports.cpp` 拆到 `ffi_player_commands`，并增加 focused native 单测。
- [x] 更新 `native/docs/FFI_AND_BINDINGS.md`：ABI v2 counted paths、status APIs、per-player error 和 legacy wrapper 规则。
- [x] 扩展 `native/tests/ffi/test_ffi_c.c` 覆盖 v1/v2 coexist、counted path、per-player error、status APIs、destroy 并发。

建议验证：

- `python dev.py test --native-only --github` build passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R test_ffi_c` passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R video_renderer_tests` passed on 2026-05-10.

## P0 - Unified Renderer Config Validation

Status: done in Round 11.

目标：所有入口共享同一套配置规则，避免 FFI、MethodChannel、Python、Renderer 各自判断。

证据：

- FFI 的 `validate_player_config()` 只检查 ABI、log、width/height positive。
- MethodChannel 在 `windows/runner/video_renderer_plugin.cpp` 中独立校验 width/height <= 16384、speed、layout。
- `FrameConverter` 有单独 `kMaxDecodedDimension = 16384` 和 `kMaxCpuFrameBytes = 1GB`。
- 当前只有 `native/video_renderer/layout_validation.h` 针对 layout 做了共享校验。

TODO:

- [x] 新增 `native/video_renderer/renderer_config_validation.h/.cpp`。
- [x] 定义集中预算常量：max width/height、max path count、max tracks、max path bytes、max CPU frame bytes、max speed、loop range 规则；queued frames / deeper resource budgets 留给 P2 resource budget policy。
- [x] 输出结构化 `RendererConfigValidationResult { ok, code, message }`，供 C++/FFI/runner/Python 复用。
- [x] FFI `validate_player_config()` 改为转换后调用统一 validator，并限制 NULL-terminated path scan 上限。
- [x] MethodChannel `createPlayer` / `resize` / `setSpeed` / `setLoopRange` / `applyLayout` 改用统一 validator 或同源 helper。
- [x] Python bindings 继续走统一 layout/config validation。
- [x] 增加 config validation 单元测试，覆盖边界、path count、headless/hwnd/backend adapter 组合；非法 UTF-8 留给 P2 strict UTF-8 path round。

建议验证：

- `python dev.py test --native-only --github` build passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R video_renderer_tests` passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R test_ffi_c` passed on 2026-05-10.
- Python binding validation smoke passed from `native/build-msvc/dist/python` on 2026-05-10.
- Required runner validation `python dev.py ui-test --build ui_tests/smoke/basic.csv` was blocked before Flutter build by missing local `native/analysis/vendor/ffmpeg/voidplayer/build_windows_msvc.ps1`.
- Direct `flutter build windows --release` was blocked by the same missing FFmpeg analysis tool script during Windows CMake generation.

## P0 - NativePlayer Lifecycle State Machine

Status: done in Round 10.

目标：initialize 失败不应留下音频/playback/session 副作用，重复 initialize/shutdown 行为可预测。

证据：

- `native/player/native_player.cpp` 中 `initialize()` 先 `playback_.start_session()`，再 `renderer_.initialize(config)`。
- `Renderer::initialize()` 会拒绝 already initialized/running/joinable，但这发生在 `NativePlayer` 已触碰 playback 之后。

TODO:

- [x] 给 `NativePlayer` 增加最小生命周期状态锁：`Created -> Initializing -> Initialized -> ShuttingDown`。
- [x] 在 `NativePlayer::initialize()` 先检查当前状态和 renderer 状态，再启动 playback session。
- [x] initialize 失败时完整 rollback，保证 repeated initialize failure 不改变既有 initialized session。
- [x] 明确 `shutdown()` 的幂等语义和并发调用策略；保持 FFI per-handle mutex 下的行为简单。
- [x] 增加 native tests：duplicate initialize preserves active playback session，failed initialize rollback and retry。

建议验证：

- `python dev.py test --native-only --github` build passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R video_renderer_tests` passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R test_ffi_c` passed on 2026-05-10.
- Full `python dev.py test --native-only` was blocked before native build/test by missing local `native/analysis/vendor/ffmpeg/voidplayer/build_windows_msvc.ps1`; use `--github` or restore the analyzer submodule/tooling for the full local analysis path.

## P1 - Renderer Split / Concurrency Safe Order

目标：逐步拆掉 God Object，但每轮只移动一个低争议边界，避免破坏 D3D/Texture/seek 交叉状态。

证据：

- `Renderer` 当前同时拥有 D3D backend/raw resource pointers、layout、analysis overlay pixels、capture、render loop、track manager、seek/audio coordinator、perf metrics、device lock、texture lock。
- `native/docs/THREADING_MODEL.md` 已记录 lock order，可作为拆分护栏。
- `build/chat/review_renderer.md` 的新核验属实：当前风险已经从“职责边界还大”升级为“Renderer 状态访问模型和锁契约不一致”。
- `native/video_renderer/` 根目录已有 40+ 个 renderer 相关文件，policy/helper 平铺已经降低可审查性；但目录重组应等并发修复落地后单独做 mechanical patch。

新增优先队列（来自 `build/chat/review_renderer.md`）：

1. [x] `RendererDrawSnapshotLockBoundary`
   - 目标：`draw_frame()` 不再拿 `state_mutex_`，也不直接读 `tracks_` / `layout_` / `background_color_`；进入 `device_mutex_` 前完成 immutable draw snapshot。
   - 验证：native-only + smoke/viewport/analysis UI。

2. [x] `RendererRenderLoopStateSnapshotBoundary`
   - 目标：render loop policy helpers 不再直接吃 mutable `TrackPipelineManager&`；`last_decision_`、`preview_drawn_`、`was_buffering_` 的读写收口到明确状态边界。
   - 验证：native-only + smoke/seek/timeline UI。

3. [x] `RendererFramePresenterSerializationBoundary`
   - 目标：`D3D11FramePresenter::prepare_frame/reset_track/move_track/memory_stats` 对 slot resources 的访问统一串行化；优先考虑 render-thread command queue 或同一 device-side boundary。
   - 验证：native-only + smoke/track compact/seek UI。

4. [ ] `RendererQueryLockBoundary`
   - 目标：`track_count()`、`duration_us()`、`has_track()`、`track_dimensions()`、`track_infos()` 统一 `state_mutex_` 保护，不能依赖 NativePlayer shared lock。
   - 验证：native-only。

5. [ ] `RendererShutdownCallbackAndLoopGuard`
   - 目标：shutdown 开始后 gate late demux/render callbacks；render loop 使用 RAII timer guard 和 noexcept exception boundary；退出时不再无条件 flush pending resize。
   - 验证：native-only + shutdown/recreate UI smoke。

6. [ ] `RendererBackendRefsCleanup`
   - 目标：清掉未使用或可由 `RenderBackend` 访问的 borrowed raw backend pointers，降低 shutdown/reinit 悬空指针心智负担。
   - 验证：native-only + smoke UI。

7. [ ] `RendererDirectoryRegroup`
   - 目标：并发修复稳定后，把 renderer policy/helper 文件按 `render/`、`layout/`、`track/`、`seek/`、`stats/` 等域分层；`renderer.cpp/h` 暂留 root 作为 facade/owner。
   - 验证：CMake/Flutter runner build + native-only；该 patch 只做 include/source-list move，不混入行为变化。

建议顺序：

1. [ ] `FrameCaptureService`
   - 输入：`D3D11HeadlessOutput` / published front buffer / WIC 或 BGRA copy helper。
   - 从 `Renderer::capture_front_buffer()` 和 runner PNG capture 中切出 native-facing capture service。
   - 验证：native test + `ui_tests/smoke/basic.csv` 中 capture/hash 相关路径。

2. [ ] `LayoutController`
   - 持有 `LayoutState`、background color、display pixel size、order mapping validation glue。
   - 先抽 pure state/helper，不移动 render-thread D3D draw。
   - 验证：`python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/viewport_pan_layout_regression.csv`

3. [ ] `AnalysisOverlayRenderer`
   - 把 overlay texture、pixel cache、draw_analysis_overlay/ensure texture 从 Renderer 拆出。
   - 目标是后续让 analysis feature 可选。
   - 验证：`python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv`

4. [ ] `RenderLoopController`
   - 最后拆，因它触碰 running/thread/join/callback/device lock。
   - 先只封装 start/stop/join/publish callback，不改变 draw decision 逻辑。
   - 验证：native-only + smoke + timeline/seek 相关脚本。

5. [ ] `DeviceLossPolicy`
   - 集中 terminal/lost state、removed reason、metrics、future recovery policy。
   - 验证：能单测 policy，D3D 真 device lost 作为后续手工/模拟测试。

6. [x] `TrackPipelineFactory` / `TrackLifecycle`
   - 已把 demux/decode pipeline construction 拆到 `TrackPipelineFactory`。
   - 已把 file_id/offset/recreate flags、seek/error/audio hooks、demux start、failed-start rollback 拆到 `track_lifecycle`。
   - 验证：native track pipeline/lifecycle tests + smoke/track compact/shutdown-during-seek recreate UI。

7. [x] `TrackRemovalCompaction`
   - 已把 remove-track stop/compact/render-sink/presenter slot side effects 和 cached present decision frame compaction 拆到 `track_lifecycle`。
   - 验证：native track lifecycle tests + smoke/track compact UI。

8. [x] `AddTrackSeekPolicy`
   - 已把 add-track 对齐当前播放时钟时的 seek target clamp、buffer/audio queue flush、audio pause、seek type choice 拆到 `track_lifecycle`。
   - 验证：native-only + smoke/track compact UI。

9. [x] `HevcSeekRecreatePolicy`
   - 已把 `seek_internal` 内 HEVC hardware seek recreate/coalesce/error 选择拆到 `SeekCoordinator` policy。
   - 验证：native-only + smoke/shutdown-during-seek recreate UI。

10. [x] `TrackSeekPreparation`
   - 已把 `seek_internal` 内每轨 seek 前的 decode/audio pause、buffer clear、presenter reset、packet/audio queue flush、seek request submission 拆成 lifecycle helper。
   - 验证：native-only + smoke/timeline seek/shutdown-during-seek recreate UI。

11. [x] `SeekPipelineRecreateLifecycle`
   - 已把 `recreate_pipeline_for_seek` 的 stop/recreate/start/render-sink commit 编排拆成 lifecycle helper，并清理不再使用的 decode-thread-only recreate 路径。
   - 验证：native-only + smoke/shutdown-during-seek recreate UI。

12. [x] `TrackAddCommit`
   - 已把 add-track 的 render-sink/frame-presenter/tracks slot commit 拆成 lifecycle helper，Renderer 保留 layout/duration/playback 决策。
   - 验证：native-only + smoke/track compact UI。

13. [x] `TrackDurationCache`
   - 已把 initialize/add/remove 中的 track duration max/recompute 逻辑拆成可单测 helper，Renderer 只保留 cached value。
   - 验证：native-only + smoke/track compact UI。

14. [x] `TrackPlaybackPauseGuard`
   - 已把 add/remove track 中临时暂停播放、失败回滚、成功后按条件恢复播放的重复逻辑拆成 helper/policy。
   - 验证：native-only + smoke/track compact UI。

15. [x] `SeekTargetClampPolicy`
   - 已把 `seek_internal` 开头的 target clamp 与 pending seek event retarget 决策拆出，Renderer 保留 clock/deferred seek 执行。
   - 验证：native-only + smoke/seek clamp UI。

16. [x] `TrackSeekTargetPolicy`
   - 已把 `seek_internal` 内每轨 requested target / offset clamp facts 拆成 helper，Renderer 只保留日志与执行。
   - 验证：native-only + smoke/seek clamp UI。

17. [x] `TrackOffsetMutation`
   - 已把 `set_track_offset` 的轨道 offset 赋值与 render-sink offset 更新拆进 lifecycle helper，Renderer 保留 file-id lookup 与 redraw invalidation。
   - 验证：native-only + smoke/track offset refresh UI。

18. [x] `TrackManagerQueries`
   - 已把 `track_count` / first active slot 这类简单查询下放到 `TrackPipelineManager`，Renderer 保留 public API 和锁。
   - 验证：native-only + smoke/track compact UI。

19. [x] `TrackInfoSnapshot`
   - 已把 `track_infos()` 的 metadata 组装拆到 `track_snapshot` helper，Renderer 保留 public API 形状。
   - 验证：native-only + smoke/track compact UI。

20. [x] `TrackPerfSnapshot`
   - 已把 `track_perf_stats()` 的每轨字段组装拆到 `track_snapshot` helper，Renderer 保留锁、FPS baseline timing 和 public API。
   - 验证：native-only + smoke UI。

21. [x] `TrackGpuMemorySnapshot`
   - 已把 `gpu_memory_stats()` 里的每轨 GPU/memory 字段组装拆到 `track_snapshot` helper，Renderer 保留 device/state locking、aggregate totals 和 D3D resource ownership。
   - 验证：native-only + smoke UI。

22. [x] `LoopRangeSeekPolicy`
   - 已把 `apply_loop_range_locked()` 的 loop 边界判断拆成可单测 policy，Renderer 保留锁、clock 读取、日志和 `seek_internal()` 执行。
   - 验证：native-only + smoke/loop UI。

23. [x] `LoopRangeState`
   - 已把 nested `LoopRangeState` 和 set-loop normalization/comparison 拆出 Renderer，Renderer 保留 validation、锁、状态存储和日志。
   - 验证：native-only + smoke/loop UI。

24. [x] `PlaybackDecodeState`
   - 已把 public play/pause 的 track decode pause 和 pause-after-preroll fanout 拆到 helper，Renderer 保留生命周期锁、playback clock 命令、`playing_` 和 seek coordinator reset。
   - 验证：native-only + smoke UI。

25. [x] `DecodePauseFanout`
   - 已把 `set_decode_paused_for_all_tracks()` 的剩余 all-track decode/audio pause fanout 拆到 helper，Renderer 保留调用意图和锁。
   - 验证：native-only + smoke UI。

26. [x] `StepDecodePauseFanout`
   - 已把 `step_forward()` 内临时 per-track decode pause/resume fanout 拆到 helper；该路径只影响视频 decode thread，不应触碰 audio decode pause state。
   - 验证：native-only + smoke/step-forward UI。

27. [x] `StepBufferingGate`
   - 已把 `step_forward()` / `step_backward()` 共享的 Buffering 轨道阻塞判断拆到 helper，Renderer 保留锁和 step/fallback 决策。
   - 验证：native-only + smoke/step-forward UI。

28. [x] `StepBackwardRetreatFanout`
   - 已把 `step_backward()` 内 all-track `can_retreat()` / `retreat()` 扇出拆到 helper，Renderer 保留 clock seek、fallback exact seek 和 draw 决策。
   - 验证：native-only + smoke UI。

29. [x] `StepPolicyOwner`
   - 已把 step-specific helper 从 generic `track_lifecycle` 迁到独立 `track_step_policy`，避免 lifecycle helper 继续膨胀成二级 god object。
   - 验证：native-only + smoke UI。

30. [x] `StepForwardDecisionPolicy`
   - 已把 `build_step_forward_decision_locked()` 的 next-frame selection 和 `discard_step_forward_consumed_frames_locked()` 的 buffer drain 拆到 `track_step_policy`。
   - 验证：native-only + smoke/step-forward UI。

31. [x] `StepFrameDurationPolicy`
   - 已把 `compute_frame_duration_us()` 的 per-track min duration/fallback policy 拆到 `track_step_policy`。
   - 验证：native-only + smoke/step-forward UI。

32. [x] `PrerollBufferingGate`
   - 已把 render loop 中 Empty/Flushing/Buffering 任一轨道阻塞 preroll 的判断拆到 `track_preroll_policy`，Renderer 保留 clock pause/resume 和 preview invalidation。
   - 验证：native-only + smoke UI。

33. [x] `PausedPreviewSnapshot`
   - 已把 render loop 暂停预览的 ALL active tracks have frames 组装规则拆到 `track_preview_policy`，Renderer 保留 cached last-frame reuse、present、`preview_drawn_`、seek-preview event 和日志。
   - 验证：native-only + smoke UI。

34. [x] `PresentCarryForwardPolicy`
   - 已把播放态 `PresentDecision` 缺帧时复用 `last_decision_` 的 carry-forward 规则拆到 `track_present_policy`，Renderer 保留 `RenderSink::evaluate`、present、last-decision commit 和 redraw fallback。
   - 验证：native-only + smoke UI。

35. [x] `EmptyBufferEofClampPolicy`
   - 已把播放态 buffer-empty scan 和 max last-presented end PTS 计算拆到 `track_present_policy`，Renderer 保留 clock seek/clamp 与 `settle_eof_locked`。
   - 验证：native-only + smoke UI。

36. [x] `FrameDeadlineEventPolicy`
   - 已把播放态 next frame event PTS 扫描拆到 `track_present_policy`，Renderer 保留 clock read、speed、`RenderLoopController::frame_deadline_sleep` 和实际 sleep。
   - 验证：native-only + smoke UI。

37. [x] `RenderLoopDiagnosticsSnapshot`
   - 已把 render-loop periodic diagnostics 的 per-track buffer count/cap/state 快照拆到 `track_snapshot`，Renderer 保留 cadence 和日志输出。
   - 验证：native-only + smoke UI。

38. [x] `PausedFrameDrawSnapshot`
   - 已把 `draw_paused_frame()` 的 current-frame snapshot 组装拆到 `track_preview_policy`，Renderer 保留 last-decision fallback、present、clock/log ref selection 和 commit。
   - 验证：native-only + smoke UI。

39. [x] `LayoutTrackGeometrySnapshot`
   - 已把 `snapshot_layout_track_geometry` 从 `renderer.cpp` 移入 `layout_geometry`，Renderer 只传入 track manager 给 shader constants。
   - 验证：native-only + smoke/viewport UI。

40. [x] `InitialRenderSinkBinding`
   - 已把 `Renderer::initialize()` 中 active track 到 `RenderSink` 的初始 slot binding 循环拆到 `track_lifecycle` helper，Renderer 保留 render sink 构造。
   - 验证：native-only + smoke UI。

41. [x] `InitialLayoutOrder`
   - 已把 `Renderer::initialize()` 中 active track 到 layout order 的初始 append 循环拆到 `LayoutController`，Renderer 保留 layout reset 和 track manager。
   - 验证：native-only + smoke/viewport UI。

42. [x] `InitialActiveTrackQuery`
   - 已把 `Renderer::initialize()` 中剩余的 active track 扫描替换为 `TrackPipelineManager` 查询，Renderer 保留失败分支和错误日志。
   - 验证：native-only + smoke UI。

43. [x] `PerfBaselineReset`
   - 已新增 `TrackPerfBaselineTracker` 收口 stats timer 和 per-slot frame baseline reset/rotation，Renderer 不再直接持有 baseline array。
   - 验证：native-only + smoke UI。

44. [x] `InitialTrackCreation`
   - 已把 `Renderer::initialize()` 的初始 video path loop 拆入 track lifecycle helper，Renderer 保留 pipeline factory hooks、file-id 分配和错误日志上下文。
   - 验证：native-only + smoke UI。

45. [x] `ShutdownResourcePresence`
   - 已集中 shutdown 里“是否还有资源需要释放”的判断，并复用 track manager active-track query 替代本地 track scan。
   - 验证：native-only + smoke UI；第一次 native-only 命中 unrelated analysis read-count 抖动，立即重跑通过。

46. [x] `PresentDecisionFrameQuery`
   - 已把 `Renderer::has_any_frame()` 这种纯 `PresentDecision` 查询迁入 present policy，Renderer 只保留 fallback 调度。
   - 验证：native-only + smoke UI。

47. [x] `PreviewPolicyPresentDecisionQueryReuse`
   - 已移除 `track_preview_policy` 中重复的匿名 frame-presence helper，复用 present policy 的共享查询。
   - 验证：native-only + smoke UI。

48. [x] `EffectiveDurationPolicy`
   - 已把 `Renderer::effective_duration_us_locked()` 中 per-track duration/end-PTS 合成逻辑迁到 track lifecycle/duration policy，保留 cached-duration fallback。
   - 验证：native-only + smoke UI。

49. [x] `TrackGeometryUpdatePolicy`
   - 已把 `Renderer::update_track_geometry_from_decision_locked()` 的 frame-size/aspect mutation 迁入 layout/geometry helper，Renderer 保留日志。
   - 验证：native-only + smoke/viewport UI。

50. [x] `CachedPresentPtsQuery`
   - 已把 paused cached-frame 日志里的 first-frame PTS scan 迁入 present policy helper，Renderer 只负责日志格式。
   - 验证：native-only + smoke UI。

51. [x] `SeekPreviewPresentedEventCollection`
   - 已把 `Renderer::emit_seek_preview_presented_events()` 中按 slot 拼 seek-preview presented track event 的扫描迁入 present policy helper。
   - 验证：native-only + smoke/timeline UI。

52. [x] `TrackPerfStatsCollection`
   - 已把 `Renderer::track_perf_stats()` 的 active-track snapshot collection 迁入 track snapshot helper，Renderer 保留 timer/baseline ownership。
   - 验证：native-only + smoke UI。

53. [x] `TrackGpuMemoryStatsCollection`
   - 已把 `Renderer::gpu_memory_stats()` 的 per-track memory aggregation 迁入 track snapshot helper，Renderer 保留 D3D presenter/headless/overlay 聚合。
   - 验证：native-only + smoke UI。

54. [x] `AnalysisOverlayMemoryStats`
   - 已把 `Renderer::gpu_memory_stats()` 的 analysis overlay GPU resource aggregation 迁出 Renderer。
   - 验证：native-only + smoke UI。

55. [x] `TrackSeekFacts`
   - 已把 `Renderer::seek_internal()` 中 per-track target clamp / hardware decode / HEVC-HW / H.264-FLV exact-seek warning facts 集中到 track lifecycle helper。
   - 验证：native-only + smoke/seek UI。

56. [x] `TrackSeekTransitionAssembly`
   - 已把 `Renderer::seek_internal()` 中 per-track transition/recreate input assembly 迁入 track lifecycle helper，Renderer 暂保留 hooks 和实际 seek/recreate 动作。
   - 验证：native-only + smoke/seek UI。

57. [x] `TrackSeekExecutionBoundary`
   - 已把 `Renderer::seek_internal()` 中 HEVC recreate decision 后的 error/coalesce/result handling 和 seek submission 迁入 track lifecycle helper。
   - 验证：native-only + smoke/seek UI。

58. [x] `TrackSeekSlotApplication`
   - 已把 `Renderer::seek_internal()` 中 per-slot facts / transition / plan / decision / execution 串联成一个 track lifecycle 边界，Renderer 只保留 hooks、全局 seek 状态和日志。
   - 验证：native-only + smoke/seek UI。

59. [x] `StepForwardExactSeekFallback`
   - 已把 `Renderer::step_forward()` 中 cache-miss exact-seek fallback target 计算迁入 track step policy，Renderer 保留 wait loop / seek / draw / log。
   - 验证：native-only + smoke/step-forward UI。

60. [x] `StepBackwardExactSeekFallback`
   - 已把 `Renderer::step_backward()` 中 cache-miss exact-seek fallback target 计算迁入 track step policy，Renderer 保留 retreat / seek / draw / log。
   - 验证：native-only + smoke/step-backward UI。

61. [x] `StepForwardDecisionApplication`
   - 已把 `Renderer::step_forward()` 中 successful step decision 的 consumed-frame discard / reference slot / clock target 计算迁入 track step policy，Renderer 保留 wait loop / present / final log。
   - 验证：native-only + smoke/step-forward UI。

62. [x] `StepBackwardRetreatApplication`
   - 已把 `Renderer::step_backward()` 中 successful retreat 后的 reference slot / clock target 计算迁入 track step policy，Renderer 保留 retreat/fallback 分支、draw/log。
   - 验证：native-only + smoke/step-backward UI。

63. [x] `AudioEngineTrackRegistry`
   - 已把 chat 点名的 `AudioEngine::Impl` track registry / buffer publication / pause-seek fanout 容器策略拆出 Impl。
   - 验证：native-only + smoke UI。

64. [x] `AudioDecodeThreadBoundary`
   - 已继续收 `AudioEngine::Impl` God Object，把 nested audio decoder thread 从 `audio_engine.cpp` 拆到独立边界。
   - 验证：native-only + smoke UI。

65. [x] `AudioWaveOutOutputBoundary`
   - 已继续收 `AudioEngine::Impl` God Object，把 nested waveOut device/output thread 从 `audio_engine.cpp` 拆到独立边界。
   - 验证：native-only + smoke UI。

66. [x] `AnalysisManagerSessionBoundary`
   - 已继续收 chat 点名的 `AnalysisManager` 隐形 God Object，把 VAC2 session / overlay chunk index-cache / decoded chunk LRU / PTS 映射拆到 `AnalysisSession`。
   - 验证：native-only + smoke/analysis UI。

67. [x] `AnalysisOverlayTrackRegistry`
   - 已继续收 `AnalysisManager` 隐形 God Object，把 overlay track registry 从 singleton manager 中拆出，并改为保存 per-track `AnalysisSession`，消除了 recursive AnalysisManager 实例。
   - 验证：native-only + smoke/analysis UI。

68. [x] `DecodeThreadCodecLoopBoundary`
   - 新增 `codec_loop` 边界承接 FFmpeg codec send/receive 的 SEH 防护、返回值分类和硬解 device mutex 包装；`DecodeThread` 主循环不再直接散落 send/receive 包装细节。
   - 验证：native-only + seek UI。

69. [x] `DecodeThreadFramePublisherBoundary`
   - 新增 `DecodedFramePublisher` 承接硬解 visibility flush、frame conversion、push_frame 以及转换失败后的 Error/暂停/停止状态处理；`DecodeThread` 保留 exact-seek 和主循环策略。
   - 验证：native-only + seek UI。

70. [x] `DecodeThreadDrainPolicyBoundary`
   - 新增 `decode_drain_policy` 承接 drain-before-next-packet 与 EOF codec drain 的 send/receive 返回值判定、清 drain request、错误停止和暂停/flush 停止 gates。
   - 验证：native-only + seek UI。

71. [x] `DecodeThreadExactSeekCandidateStoreBoundary`
   - 新增 `ExactSeekCandidateStore` 承接 exact-seek reorder/pending 候选帧容器、候选收集规则和轻量内存统计；`DecodeThread` 保留发布、状态迁移和日志。
   - 验证：native-only + seek UI。

72. [x] `DecodeThreadTimestampRescaleBoundary`
   - 新增 `frame_timestamp_rescaler` 承接 AVFrame PTS / best-effort PTS / DTS / duration 到微秒的转换语义；decode loop 不再携带成员捕获 lambda。
   - 验证：native-only + seek UI。

73. [x] `DecodeThreadSeekEpochBoundary`
   - 新增 `decode_seek_epoch` 承接 pending seek take/reset、seek type label 和 exact/keyframe seek epoch 起始状态决策；`DecodeThread` 保留 FFmpeg flush、buffer 状态写入和日志副作用。
   - 验证：native-only + seek UI。

74. [x] `DecodeThreadPacketConsumptionBoundary`
   - 扩展 `decode_loop_policy` 承接 packet pop 分流、cancel checkpoint 和 packet send 返回值决策；`DecodeThread` 保留 AVPacket ownership、日志和状态写入。
   - 验证：native-only + seek UI。

75. [x] `DecodeThreadReceiveActionBoundary`
   - 扩展 `decode_loop_policy` 承接普通 receive loop 的 cancel gate、EAGAIN/EOF stop、硬错误日志 stop 和 SEH error stop 决策；`DecodeThread` 保留 AVFrame lifetime、exact-seek candidate 和发布流程。
   - 验证：native-only + seek UI。

76. [x] `DecodeThreadPrerollTransitionBoundary`
   - 新增 `decode_preroll_policy` 承接 post-seek 软/硬解 preroll target、普通/full preroll readiness 和 Buffering->Ready transition intent；`DecodeThread` 保留日志、TrackBuffer state 写入和 pause-after-preroll 原子状态。
   - 验证：native-only + seek UI。

77. [x] `DecodeThreadEofDrainBoundary`
   - 已把 queue gap / EOF drain 编排从 `run()` 抽到 `handle_queue_gap_or_eof()`，复用已测试的 EOF drain/send/receive policy；主循环只保留 pop 分流和 stop/continue 意图。
   - 验证：native-only + seek UI。

78. [x] `DecodeThreadExactSeekPublishBoundary`
   - 新增 `exact_seek_publish_policy` 承接 exact-seek preview 发布窗口裁剪和成功发布后的 Ready/pause/drain/target-reset 状态意图；`DecodeThread` 保留 AVFrame、硬解 wait、stable-frame 复用和转换失败副作用。
   - 验证：native-only + seek UI。

79. [x] `DecodeThreadFrameLifetimeBoundary`
   - 新增 `AvFrameUnrefGuard` / `reset_reusable_av_frame` 收口 receive 成功后的可复用 AVFrame unref 时机；`DecodeThread` 不再在 normal receive、EOF drain、post-preview drain、exact-seek candidate 分支散落手动 unref。
   - 验证：native-only + seek UI。

80. [x] `DecodeThreadPublishErrorBoundary`
   - 扩展 `DecodedFramePublisher::push_converted_frame`，让 exact-seek stable-frame / converted-frame 发布复用 normal publish 的 Error/pause/running 状态语义；`DecodeThread::publish_exact_seek_window` 不再直接写 conversion-failure 状态。
   - 验证：native-only + seek UI。

81. [x] `DecodeThreadExactSeekFramePublisherBoundary`
   - 新增 `exact_seek_frame_publisher` 承接 exact-seek preview window 帧发布、pending candidate 发布、硬解 wait/flush、stable-frame 复用和转换失败清理；`DecodeThread` 保留发布调度、成功后的 post_seek/drain/pause 状态收尾和日志。
   - 验证：native-only + seek UI。

82. [x] `FfiLifecycleShellBoundary`
   - 新增 `ffi_player_lifecycle` 承接 create/destroy/error copy、initialize v1/v2 和 shutdown lifecycle command bodies；`ffi_exports.cpp` 保留 ABI 函数名和 `ffi_guard` 外壳。
   - 验证：native-only。

83. [x] `FfiProcessGlobalShellBoundary`
   - 新增 `ffi_process_globals` 承接 process-wide logging/crash handler convenience command bodies；`ffi_exports.cpp` 只保留对应 ABI guard shims。
   - 验证：native-only。

84. [x] `RendererPlaybackCommandBoundary`
   - 新增 `renderer_playback_command_policy` 承接 play/pause/step 的 deterministic command plan；`Renderer` 保留生命周期锁、playback clock ownership、seek reset 和 decode fanout 执行。
   - 验证：native-only + smoke/seek UI。

85. [x] `RendererSeekClockBoundary`
   - 扩展 `SeekCoordinator` 纯策略层，承接 `seek_internal` 的 clock target 和 paused HEVC exact-seek deferred gate eligibility；`Renderer` 保留 playback clock mutation、coordinator 状态 mutation 和 per-track seek 执行。
   - 验证：native-only + smoke/seek UI。

86. [x] `RendererSeekLoggingBoundary`
   - 新增 `renderer_seek_log_policy` 承接 seek request/clamp、per-track target clamp、HEVC coalescing、cleared-track diagnostics 的 facts assembly；`Renderer` 保留日志发射时机和 seek side effects。
   - 验证：native-only + smoke/seek UI。

87. [x] `RendererResizeLayoutMutationBoundary`
   - 新增 `adjust_layout_view_offset_for_resize`，把 headless resize 时按旧/新 display size 缩放 `view_offset` 的 layout mutation 迁入 `layout_geometry`；`Renderer` 保留锁、target dimensions 和 D3D output resize。
   - 验证：native-only + smoke/viewport UI。

88. [x] `DecodeThreadPostPreviewCompletionBoundary`
   - 扩展 `exact_seek_publish_policy`，承接 exact-seek preview publish 成功 gate、pause/drain state facts 以及 completion log counters；`DecodeThread` 保留 TrackBuffer 状态写入、atomics 和日志发射。
   - 验证：native-only + smoke/seek UI。

89. [x] `DecodeThreadExactSeekPublishSchedulingBoundary`
   - 扩展 `decode_loop_policy`，承接 exact-seek candidate 收集后的 preview publish gate 和硬解 exact-seek pacing gate；`DecodeThread` 保留 candidate ownership、publish 调用、sleep 和 loop control。
   - 验证：native-only + smoke/seek UI。

## P1 - Windows Runner Plugin Split

目标：把 `video_renderer_plugin.cpp` 从“第二个 Renderer”拆成可审查的 app bridge。

证据：

- 文件约 1415 行；同一文件仍处理 MethodChannel handlers、player lifecycle 和 FFI diagnostics export。
- `NativePlayerRegistry` 仍是 FFI diagnostics 的 process-global player stats 入口，多 engine/multi player 语义仍需继续收口。

TODO:

- [x] 新增 `NativePlayerMethodDispatcher`：只负责 MethodChannel method -> typed handler 分发。
- [x] 新增 `FlutterTextureBridge`：收口 texture registrar、shared handle acquire/release、frame callback。
- [x] 新增 `NativeDiagnosticsProvider`：先收口 process memory、heap、DXGI dedicated memory 查询，保持 MethodChannel/FFI payload 不变。
- [x] 将 MethodChannel native/player diagnostics 聚合进 `NativeDiagnosticsProvider`，保持返回 payload 不变。
- [x] 将 FFI flat diagnostics 聚合进 `NativeDiagnosticsProvider`，保持 `naki_vr_get_diagnostics` ABI 不变。
- [x] 将 MethodChannel diagnostics active-player lookup 收口到 plugin instance scope。
- [ ] 将 FFI diagnostics active-player lookup 从 process-global player registry 收口到 host/session scope。
- [x] 新增 `NativeLoggingBootstrap`：收口 runner 默认日志路径、log file 清洗、native logging reconfigure、startup trace flush 和 crash handler opt-in。
- [x] 新增 `ViewportCaptureService`：PNG/WIC save 和 BGRA hash/preview 归一。
- [x] 新增 `FilePickerService`：收口 Windows file dialog / video filter / UTF-16 path conversion。
- [ ] 分批迁移，每批保持 MethodChannel payload 不变。

建议验证：

- `python dev.py test --native-only`
- `python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv`

## P1 - Global State Isolation

目标：降低 process-global 状态对多 engine、多 player、测试隔离和卸载重载的影响。

证据：

- `windows/runner/native_player_registry.*` 仍提供 process-global active player registry。
- `windows/runner/analysis_ffi.cpp` 仍有 atomic global PTS callback 和 handle registry；legacy singleton reader API 已移除，overlay state 仍是 renderer-facing global。
- FFI logging/crash convenience API 已在 public header 标明 process-global ownership；host-provided logger/sink 长期接口仍待设计。

TODO:

- [x] 把裸 `g_player_weak` / `g_player_mutex` 收口到 `NativePlayerRegistry`。
- [x] 把 MethodChannel diagnostics 的 active player 从 process-global registry 改为 plugin instance scope。
- [ ] 把 FFI diagnostics 的 active player 从 process-global registry 改为 host/session scope。
- [ ] analysis PTS callback 支持 handle/player scoped 注册；global callback 标记 deprecated。
- [x] 移除 analysis legacy singleton reader API；Dart/native 读取路径改为 handle-scoped VAC2 session。
- [x] 为 process-global logging/crash FFI API 增加文档警示。
- [ ] 规划 host-provided logger/sink 的长期接口。
- [x] 增加 repeated create-destroy UI smoke，覆盖 last-track remove -> destroy -> recreate。
- [ ] 增加 multi player / plugin teardown smoke。

建议验证：

- `python dev.py test --native-only`
- analysis 相关改动：`python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h264.csv`

## P1 - Decode / Seek / Render Stress Tests

Status: partially done in Round 17.

目标：把高风险并发区从“靠经验”变成可重复压力测试。

证据：

- PacketQueue/BidiRingBuffer 已有随机/property-style 覆盖。
- DecodeThread 仍包含硬解 fallback、exact seek reorder、paused seek preview、drain/flush/cancel/pause_after_preroll/post_seek 等复杂状态。
- Renderer 涉及 render thread、decode/demux threads、D3D immediate context lock、Texture callback 和 shutdown。

TODO:

- [x] 新增 deterministic stress test harness：固定 seed，随机 play/pause/seek/step/speed/loop/layout 控制序列。resize/capture/add/remove/shutdown 并发仍待后续专门测试。
- [x] 先跑 native 层，不依赖 Flutter UI。Round 17 使用 windowed software renderer；headless capture stress 留给后续。
- [x] 给 DecodeThread exact seek lookbehind / preview-window selection 增加更小粒度状态机单测。
- [x] 给 DecodeThread pending/drain/pause/stale-packet loop guards 增加更小粒度状态机单测。
- [x] 给 DecodeThread EOF drain/exact-seek EOF publish decisions 增加更小粒度状态机单测。
- [ ] 给 DecodeThread codec send/receive、AVFrame ownership 和硬解 visibility flush 增加更小粒度边界测试。
- [x] 增加 shutdown during seek + recreate UI smoke。
- [ ] 给 Renderer shutdown during capture/resize 增加 smoke/stress。
- [x] 记录每个 stress test 的 seed 和失败复现命令入口。
- [ ] 后续再接入 sanitizer/clang-cl 或 Windows Application Verifier。

建议验证：

- `python dev.py test --native-only --github` build passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R video_renderer_tests` passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R test_ffi_c` passed on 2026-05-10.

## P1 - CI / Tooling Hardening

Status: partially done in Round 15.

目标：让外部 contributor 的 clean checkout 更可预期，让 native 回归更早暴露。

证据：

- `.github/workflows/native.yml` 当前有 Windows native test、Release config matrix build、FFI ABI smoke。
- 未发现 CMakePresets、Debug CI、clang-cl、clang-tidy/cppcheck、dist load smoke、release compliance smoke。

TODO:

- [x] 增加 `CMakePresets.json`：windows-release、windows-debug、ffi-only、no-python-no-tests。
- [x] CI matrix 增加 Debug build，至少覆盖 `BUILD_FFI=ON BUILD_TESTS=ON`。
- [ ] 增加 clang-cl configure/build job，先允许独立 warning baseline，再逐步收紧。本机当前未发现 `clang-cl`。
- [ ] 增加 clang-tidy 或 cppcheck job，先限定 `native/common`、`native/player`、`native/video_renderer/exports`。本机当前未发现 `clang-tidy` / `cppcheck`。
- [x] 增加 clean dist smoke：检查 FFI DLL、header、FFmpeg DLL 和 notice 文件进入 `dist/ffi`。
- [x] 增加 release compliance smoke，和 P0 合规任务联动。

建议验证：

- `python scripts/dev/check_native_dist.py --ffi native/build-msvc/dist/ffi` passed on 2026-05-10.
- `cmake --list-presets -S native` passed on 2026-05-10.
- `cmake --preset no-python-no-tests -S native` -> `cmake --build native/build-msvc-preset-minimal --config Release --parallel` passed on 2026-05-10.
- CI-only Debug matrix needs PR confirmation.

## P2 - Target / Feature Boundary

目标：让 native 不再天然等同于“完整 Windows App 内部模块”，逐步变成可选 feature 的库集合。

证据：

- `video_renderer_core` 仍链接 FFmpeg。
- `video_renderer_lib` 固定链接 `analysis_lib`、D3D11/DXGI/d3dcompiler/winmm。
- analysis overlay 不是可选 feature。

TODO:

- [ ] 设计 target 边界：`void_core`、`void_media_ffmpeg`、`void_render_d3d11`、`void_player`、`void_analysis`、`void_ffi`、`void_flutter_windows_plugin`。
- [ ] 先增加 feature options，不急着一次重命名 target。
- [ ] 让 `BUILD_ANALYSIS=OFF` 时 renderer/player/FFI 能构建，analysis overlay API 返回 unsupported。
- [ ] 让 `BUILD_FFI=OFF`、`BUILD_PYTHON=OFF`、`BUILD_TESTS=OFF` 的安装/导出路径干净。
- [ ] 记录 public/internal target policy。

建议验证：

- `python dev.py test --native-only`
- `cmake -S native -B native/build-local-min -DBUILD_PYTHON=OFF -DBUILD_FFI=OFF -DBUILD_TESTS=OFF -DBUILD_ANALYSIS=OFF`

## P2 - Strict UTF-8 Paths

Status: done in Round 14.

目标：非法路径编码要早失败、可诊断，而不是变成空 path 或模糊 open failed。

证据：

- `native/common/win_utf8.h` 使用 `CP_UTF8, 0`，没有 `MB_ERR_INVALID_CHARS`。
- C ABI 和 MethodChannel 都会接收用户路径。

TODO:

- [x] 将 `utf16_from_utf8()` 改为 strict 版本，使用 `MB_ERR_INVALID_CHARS`。
- [x] 提供能返回错误原因的 API：`try_utf16_from_utf8()` / `try_utf8_from_utf16()`，避免空字符串同时表示空输入和转换失败。
- [x] FFI/path validation 把非法 UTF-8 映射成明确 `NAKI_VR_ERR_INVALID_ARGUMENT`。
- [x] runner file/path helper 同步 strict 行为，保留 Windows API 返回路径转 UTF-8 的诊断。
- [x] 增加非法 UTF-8、空路径、path count 测试；超长路径由 `kMaxRendererPathBytes` validation 覆盖入口规则。

建议验证：

- `python dev.py test --native-only --github` build passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R video_renderer_tests` passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R test_ffi_c` passed on 2026-05-10.
- Runner UI validation remains blocked by the missing local FFmpeg analysis tool script noted in Round 11.

## P2 - Resource Budget Policy

Status: partially done in Round 16.

目标：把“别炸内存”的保险丝升级成统一、可配置、可测试的资源预算。

证据：

- `FrameConverter` 有 `kMaxCpuFrameBytes = 1GB`，更像 overflow guard，不像实际产品预算。
- track count、queued frames、exact seek reorder、capture、analysis cache/file size 等预算分散或隐含。

TODO:

- [x] 定义 `NativeResourceBudget` 和默认预算入口；显式 app/FFI/Python override 留给后续配置 API。
- [x] 预算覆盖：TrackBuffer queued-frame depth 由 `TrackBufferBudget` 根据 `NativeResourceBudget` 统一决策。
- [ ] 预算覆盖：max packet queue capacity、max exact seek reorder bytes、max analysis file/cache size 和 runtime override。Round 16 已集中 max tracks / dimensions / path bytes / CPU frame bytes / capture bytes / exact seek reorder frames / speed 常量；P45 已集中 TrackBuffer queued-frame depth。
- [ ] 超预算返回明确错误码和日志。
- [ ] diagnostics 输出预算命中/拒绝计数。
- [x] 增加预算边界测试。

建议验证：

- `python dev.py test --native-only --github` build passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R video_renderer_tests` passed on 2026-05-10.
- `ctest --test-dir native/build-msvc -C Release --output-on-failure -R test_ffi_c` passed on 2026-05-10.

## Historical Completed Rounds

旧 tracker 中已完成的轮次保留为摘要，避免重复做已落地的债：

- [x] Round 1: Renderer FFI handle lifetime，使用 pinned handle state registry。
- [x] Round 2: Logging / crash runtime boundary，runner 显式 opt-in process-global 行为。
- [x] Round 3: CI config matrix 从 configure-only 提升到 configure + build，并加 FFI smoke。
- [x] Round 4: Dependency lock / third-party manifest。
- [x] Round 5: Analysis FFI ABI/error model 初步强化。
- [x] Round 6: Parser / Queue property-style hardening。
- [x] Round 7: Renderer thread contract 文档和拆分候选边界。
- [x] Round 8: Python binding ergonomics。
- [x] Round 9: Seek edge cases 文档化。
- [x] Round 10: NativePlayer lifecycle state machine and playback-session rollback。
- [x] Round 11: Unified renderer config validation across native, FFI, Python, and runner MethodChannel。
- [x] Round 12: C FFI ABI v2 counted paths, status APIs, and per-player error state。
- [x] Round 13: Release compliance notices, package smoke, and CI source notice check。
- [x] Round 14: Strict UTF-8 conversion and path validation。
- [x] Round 15: CMake presets, Debug CI matrix, and FFI dist smoke。
- [x] Round 16: NativeResourceBudget default guardrail definition。
- [x] Round 17: Deterministic renderer playback-control stress test。

后续每完成一个 P0/P1/P2 项，应把本文件对应 checklist 打勾，并在相关文档记录验证命令和日期。
