# Native Refactor Todo

Source review: `build/chat_native_adv.md` (local build artifact, static review, not a verified build/test report).

目标不是一次性重写 native，而是按风险拆成可独立验证的修复轮次。每轮开始前要先在当前代码中确认问题真实存在；每轮结束都要同步相关文档，并按 [MAINTENANCE.md](MAINTENANCE.md) 的 native 验证要求跑测试。

## 分层原则

| 优先级 | 判定标准 |
|--------|----------|
| P0 | 会阻塞干净构建、开源合规、FFI 边界稳定、用户直接黑屏/无声/崩溃的问题 |
| P1 | 会持续放大维护成本，或缺少能抓住关键回归的测试 |
| P2 | 代码味道、诊断质量、长期工程化改进 |

## Round 1 - Build Hygiene

- [x] `native/build.py` 仅在 `build_python=True` 时 import `pybind11` 并读取 `get_cmake_dir()`。
- [x] 将 `BUILD_FFI`、`BUILD_PYTHON`、`BUILD_TESTS`、`BUILD_BENCHMARKS` 等构建开关统一成显式 CMake `option()`，移除隐式默认判断。
- [x] install/staging 输出到 build tree 或 `CMAKE_INSTALL_PREFIX`，不再写入 `native/dist` 这类 source tree 路径。
- [x] 收敛 spdlog、Catch2、zstd、FFmpeg 的依赖入口，至少文档化唯一优先路径和 fallback 规则。
- [x] 增加 clean Windows build 说明或 CI 入口，覆盖 `BUILD_PYTHON=OFF`、`BUILD_FFI=ON`、`BUILD_TESTS=ON/OFF`。
- [x] 验证：`python dev.py test --native-only`。

## Round 2 - FFmpeg License And Runtime Packaging

- [x] 记录随项目使用的 FFmpeg 版本、来源、configure flags、GPL/nonfree 状态。
- [x] 随 FFmpeg DLL 同步复制 license、notice、source offer 或源码获取说明。
- [x] 明确 native DLL 与 FFmpeg 的动态链接关系，以及用户替换 FFmpeg DLL 的支持边界。
- [x] 对齐 `FFMPEG_RUNTIME_DLL_PATTERNS` 与实际链接库，避免查找了 `swscale` 但运行时复制规则不一致。
- [ ] 验证：检查 release/staging 输出目录包含 DLL 与对应 license/notice 文件。

## Round 3 - Stable C ABI And Error Model

- [x] 增加 `NAKI_VR_ABI_VERSION` 与 `naki_vr_abi_version()`。
- [x] 所有跨 FFI 的 config struct 增加 `size`、`abi_version`，并在入口处校验。
- [x] 引入项目自有的 `naki_vr_status_t`，避免把 `spdlog` enum 或 C++ enum 直接暴露成 ABI 契约。
- [x] 为 log level、seek type、layout mode、track order 等 FFI 参数增加范围校验。
- [x] 增加 `last_error` 或等价错误查询 API，让 Dart 能区分参数非法、FFmpeg 打开失败、D3D device lost、shader 失败和内部异常。
- [x] 更新 Dart FFI/MethodChannel 侧错误映射和用户可见错误文本。
  当前 Flutter 主播放器走 Windows MethodChannel，不直接消费 `video_renderer_ffi`；MethodChannel 已有 `BAD_ARGS` / `INVALID_ARGS` 映射。本轮新增的 `naki_vr_last_error()` 服务 C FFI 消费者，后续若 Dart 改为直接绑定该 DLL，需要在 Dart FFI wrapper 中读取 status/message。
- [x] 验证：补充 `native/tests/ffi` 的 struct size、ABI version、null pointer、invalid enum、double destroy 行为测试；如影响 Flutter action，追加一条 UI smoke。

## Round 4 - Frame Conversion Failure As Explicit Error

- [x] 将 `FrameConverter::convert()` 从默认空 `TextureFrame` 改为 `Result`/`optional`/错误对象。
- [x] unsupported pixel format、hwdownload 失败、CPU NV12 转换失败必须进入明确错误路径。
- [x] DecodeThread 将转换错误写入 `TrackState::Error`，不要把空 texture frame 塞进 buffer。
- [x] 通过 FFI/native player facade 把 track 错误暴露给 Flutter。
  TrackState::Error 会进入 `track_perf_stats()`，Windows MethodChannel `getDiagnostics` 和 stats-window FFI 都已暴露 `bufferState`。
- [x] 决定是否接入 libswscale/libyuv 作为兜底；如果暂不支持，文档写清楚支持的像素格式。
  当前播放器 runtime 不引入 libswscale；支持格式写入 [DECODE_PIPELINE.md](DECODE_PIPELINE.md)。
- [x] 验证：补充 unsupported pixel format/failure injection 测试；跑 `python dev.py test --native-only`，涉及上屏错误文案时跑 `python dev.py ui-test ui_tests/smoke/basic.csv`。

## Round 5 - Audio Sync Foundation

- [x] PCM 输出队列携带 PTS、duration、stream serial/range，不再只传裸 PCM bytes。
- [x] `AudioDecodeThread::notify_seek()` 使用 seek target/type，seek 后按目标 PTS 丢弃、补 silence 或重新对齐。
- [x] 明确 master clock 策略：音频跟随外部播放 Clock / 视频渲染时钟；新增 underrun、seek trim、silence、gap drift metrics。
- [x] 检查 `waveOutPrepareHeader`、`waveOutWrite`、`waveOutUnprepareHeader`、device open/reset/close 的返回值并写入 native log。
- [x] 规划 WASAPI shared mode 迁移；本轮保留 waveOut，但时间模型已抽到 `audio/pcm_buffer.*`，可被 WASAPI 后端复用。
- [x] 验证：新增 `PcmBuffer` seek/underrun/drift native 测试；本轮未改 Flutter 交互入口，未跑 UI 脚本。

## Round 6 - D3D Headless Texture And Device-Lost Handling

- [x] `D3D11HeadlessOutput::create_shared_buffers()` 获取 shared handle 失败时 hard fail，不再 warning 后继续初始化。
- [x] 缩短 texture mutex 持有范围，避免在锁内等待 GPU idle 或执行可能阻塞的 publish 流程。
- [x] 明确 Flutter texture consumer 与 native producer 的同步契约；本轮文档化现有 D3D11 query fence + texture registrar AddRef/release_callback 生命周期协议。
- [x] device lost 时至少做到停止 render loop、上报 Dart、允许 destroy/recreate；现有 `enter_terminal_device_lost_locked()` 会停 render/playback，`d3d_device_lost()` 已通过 Windows diagnostics 暴露，shutdown 后可 recreate。
- [x] 统一 D3D texture/shader/presenter 的 HRESULT 失败上报，避免只散落 log；本轮保持 `D3D11Device::record_device_error()` 作为 device-lost 收敛入口，并把 headless shared-handle 失败纳入 hard failure。
- [x] 验证：新增 shared handle fail 故障注入测试；既有 shader compile fail、device removed/poll 测试继续覆盖。已跑 `python dev.py test --native-only`；本轮未改 codec 上屏语义，未跑 HEVC/AV1/VP9 UI 回归。

## Round 7 - Renderer Responsibility Split

- [x] 从 `Renderer` 抽出 `TrackPipelineManager`，负责 track slot ownership、查找、demux/decode pipeline 创建、stop/reset/compact lifecycle primitives；Renderer 保留 playback/layout/audio/render-sink 编排。
- [x] 抽出 `SeekCoordinator`，集中处理 paused HEVC deferred seek、exact/keyframe gate 和 settle/in-flight 状态；实际 per-track seek 执行仍在 Renderer，等待 TrackPipelineManager 拆分。
- [x] 抽出 `D3D11RenderBackend`，收拢 device、shader、presenter、headless output 的创建/销毁。
- [x] 抽出 `AudioCoordinator`，收拢 audio track registration、sync、output backend。
- [ ] 保留 `Renderer` 作为生命周期和 playback/render state 的薄入口，避免继续堆 flags。
- [x] 验证：`AudioCoordinator`、`SeekCoordinator`、`D3D11RenderBackend` 拆分均已分别跑 `python dev.py test --native-only`；后续 TrackPipeline 拆分继续独立测试提交。

## Round 8 - Shader Layout And Diagnostics

- [x] 给 C++ HLSL constants struct 增加 `static_assert(sizeof(...) == 304)` 与关键 offset 校验。
- [x] 评估用 schema/generator 或 shader reflection 校验 cbuffer layout；本轮先集中到 `shader_constants.h` 并用单测锁住，后续若 shader 继续扩展再引入 reflection/generator。
- [x] runtime shader compile 依赖 `D3DCompile` 的分发策略写入文档；中期评估预编译 shader。
- [x] `RenderSink` 的 PTS tolerance 从硬编码常量升级为命名配置或有测试覆盖的常量。
- [x] 验证：补充 shader layout 单测和 RenderSink tolerance 边界测试。

## Round 9 - Library Boundary And Global Hooks

- [x] `configure_logging()` 不再清空宿主 spdlog default logger sinks；只替换 VoidPlayer native 自己持有的 sinks。
- [x] crash handler、SEH/VEH、DbgHelp、purecall/invalid parameter handler 改成显式 opt-in；Renderer 初始化不再因 `file_path` 自动安装全局 hook。
- [x] `LogConfig.max_file_size/max_files` 实现 Windows UTF-8 file sink rotation，避免 API 行为不一致。
- [x] 验证：新增 logging 初始化保留宿主 sink 测试和 rotation 测试。

## Round 10 - Ownership And Queue Result Cleanup

- [x] `DemuxStats` 中的 `AVCodecParameters` 改为 deep copy，避免 borrowed pointer 生命周期悬垂。
- [x] `PacketQueue::pop()` 返回 richer enum，区分 packet、flush、EOF、abort、empty。
- [x] `D3D11Device` release 构建默认不使用 reference driver fallback，reference 仅作为 debug option。
- [x] 清理未调用的 static helper，例如确认 `report_live_objects()` 是否接入 debug shutdown 或删除。
- [x] 验证：补充 demux pipeline recreate、queue flush/abort/reset 的单元测试；已跑 `python dev.py test --native-only`。

## 后续测试矩阵

- [ ] Windows Debug/Release clean build。
- [ ] `BUILD_PYTHON=OFF`、`BUILD_FFI=ON`、`BUILD_TESTS=ON/OFF` 组合。
- [ ] clang-cl ASan 或等价内存检查覆盖非 D3D 核心模块。
- [ ] MSVC `/analyze` 或 clang-tidy 覆盖 media/buffer/sync/FFI。
- [ ] fuzz/property tests 覆盖 `BidiRingBuffer`、`PacketQueue`、analysis parsers。
- [ ] failure injection 覆盖 FFmpeg open fail、unsupported pixel format、D3D device removed、shared handle fail、shader compile fail、audio device open fail。
- [ ] ABI tests 覆盖 struct size、enum 值、null pointer、invalid enum、invalid UTF-8/path、double destroy。
