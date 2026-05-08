# 构建与测试

## 入口命令

项目根目录下优先使用 `dev.py`，它会串起 native 构建、CTest 和 Flutter 侧需要的产物复制。

```bash
python dev.py build --native
python dev.py test
python dev.py test --native-only
python dev.py ui-test ui_tests/smoke_basic.csv
```

Native 子目录也可以单独运行：

```bash
python native/build.py
python native/build.py --build-only
python native/build.py --test-only
python native/build.py --benchmarks-only
python native/build.py --debug
```

Native FFI/Python staging 产物默认写入 `native/build-msvc/dist/`。不要依赖或提交源码树下的 `native/dist/`。

`dev.py build --native` 会在 native CMake 构建前检查 analysis 外部工具：

- VTM `DecoderApp.exe`：如果缺失，或构建 stamp 与当前 `native/analysis/vendor/vtm`、`zstd` 子模块版本不一致，会自动执行 `python dev.py vtm build` 等价流程重编。
- FFmpeg `void_ffmpeg_analyzer.exe`：如果缺失，或构建 stamp 与当前 `native/analysis/vendor/ffmpeg`、`zstd` 子模块版本 / `build_windows_msvc.ps1` 不一致，会自动运行 `native/analysis/vendor/ffmpeg/voidplayer/build_windows_msvc.ps1` 重编。

这意味着 cherry-pick 只改变 vendor 子模块指针时，下一次 `python dev.py build --native` 会自动刷新对应的小工具，不需要手动记一条额外命令。

如果 FFmpeg 不在默认的 `windows/libs/ffmpeg`，可以显式指定：

```bash
python native/build.py --ffmpeg-root <ffmpeg-root>
```

PowerShell 中也可以用环境变量：

```powershell
$env:FFMPEG_ROOT = "<ffmpeg-root>"
python native/build.py
```

## CMake 目标

| 目标 | 类型 | 说明 |
|------|------|------|
| `video_renderer_lib` | STATIC | 核心渲染/解码/同步管线 |
| `video_renderer_ffi` | SHARED | C FFI DLL，导出 `naki_vr_*` |
| `video_renderer_native` | MODULE | Python 扩展 `.pyd`，供 demo/脚本调用 |
| `video_renderer_tests` | EXE | Catch2 renderer 单元/集成测试 |
| `analysis_tests` | EXE | H.266 分析数据、VBI/VBT 解析与生成测试 |
| `analysis_generate` | EXE | AnalysisGenerator 命令行入口，供 Python 格式回归生成 VBI/VBT |
| `test_ffi_c` | EXE | C ABI smoke test |
| `probe_hw` | EXE | 硬件能力探测，存在 `probe_hw.cpp` 时构建 |
| `pipeline_bench` | EXE | 解复用/解码/上传/Present 基准，`BUILD_BENCHMARKS=ON` 时构建 |

常用 CMake 开关：

| 开关 | 默认 | 说明 |
|------|------|------|
| `BUILD_FFI` | `ON` | 构建 C FFI DLL |
| `BUILD_PYTHON` | `ON` | 构建 pybind11 Python 绑定；找不到 Python/pybind11 时自动关闭 |
| `BUILD_TESTS` | `ON` | 构建 CTest 测试目标 |
| `BUILD_BENCHMARKS` | `OFF` | 构建 pipeline benchmark |

CI 入口位于 `.github/workflows/native.yml`，包含完整 `python dev.py test --native-only`，并额外覆盖 `BUILD_PYTHON=OFF`、`BUILD_FFI=ON/OFF`、`BUILD_TESTS=ON/OFF` 的 clean configure 组合。

## 依赖

| 依赖 | 来源 | 用途 |
|------|------|------|
| FFmpeg runtime/dev package | `FFMPEG_ROOT` / `FFMPEG_DIR` / `--ffmpeg-root`，默认 `windows/libs/ffmpeg` | demux、软解、D3D11VA 硬解、hwdownload |
| zstd | `native/analysis/vendor/zstd` | VBS4 解析/生成 |
| spdlog | `build/windows/x64/_deps/spdlog-src`，缺失时 FetchContent | native 日志 |
| Catch2 | `native/_deps/catch2-src`，缺失时 FetchContent | C++ 测试 |
| pybind11 | Python 包提供的 `pybind11_DIR` 或 CMake `find_package` | Python 绑定 |
| VTM DecoderApp | `native/analysis/vendor/vtm`，缺失或 stamp 过期时由 `dev.py` 构建 | analysis 测试生成 VBS4 |

当前依赖入口以“仓库内固定路径 + 显式 override”为准：FFmpeg 只能从 `FFMPEG_ROOT` 指向的完整 dev package 读取头文件、lib 和 runtime DLL；zstd/VTM 使用 analysis vendor 子模块；spdlog/Catch2 允许本地缓存优先、网络 FetchContent 兜底。不要在同一轮改动里混入新的包管理器策略；如要迁到 vcpkg/Conan，应单独开轮次并同步 CI。

### FFmpeg Runtime Package

默认 `windows/libs/ffmpeg` 是 gyan.dev 的 FFmpeg 8.1 full shared Windows build，`README.txt` 记录来源、GPL v3 license、source commit 和完整 configure flags。VoidPlayer/native 通过 FFmpeg import libraries 动态链接这些 DLL，不静态链接 FFmpeg。用户可以用 `--ffmpeg-root`、`FFMPEG_ROOT` 或 `FFMPEG_DIR` 指向同布局的替代 FFmpeg dev package，但替换包必须同时提供 `include/`、`lib/`、`bin/` 以及对应 `README.txt`/`LICENSE`。

构建会把运行所需的 FFmpeg DLL 和 `README.txt`、`LICENSE`/`LICENSE.txt` 一起复制到 native build 输出目录、`native/build-msvc/dist/python/`、`native/build-msvc/dist/ffi/`，以及 Flutter runner 输出目录。播放器默认 runtime copy 包含 `avcodec`、`avformat`、`avutil`、`swresample`，不包含 `swscale`；只有 `BUILD_BENCHMARKS=ON` 的 `pipeline_bench` 会单独复制 `swscale`。

## `python dev.py test` 实际覆盖

`dev.py test` 会先执行 Flutter 单元测试，然后构建 native Release，再执行 `native/build.py --test-only`。当前 native 部分包含 CTest 的 3 个测试目标，随后执行 Python analysis 格式回归。

如果只想运行 native 部分，可以使用：

```bash
python dev.py test --native-only
```

| CTest | 覆盖 |
|------|------|
| `video_renderer_tests` | Clock、PacketQueue、TrackBuffer、DemuxThread、DecodeThread、FrameConverter、D3D11 device/texture/shader、RenderSink、Renderer integration，并包含 headless front-buffer capture 的 HEVC/AV1/VP9 视觉回归 |
| `analysis_tests` | H.266 分析模块，VBI/VBT 生成与解析，测试数据生成/清理 |
| `test_ffi_c` | 未初始化 renderer、空指针、基础 lifecycle、C ABI 可调用性 |

测试视频默认来自 `resources/video`，CMake 通过 `VIDEO_TEST_DIR` 注入。

补充的 Python analysis 格式回归位于 `native/analysis/tests/python/formats/`，用于校验 VBS2/VBS4/VBI/VBT 落盘格式。它会把测试视频复制到临时目录，使用 `analysis_generate.exe` 生成 VBI/VBT，再调用 `python dev.py vtm analyze --format vbs2/vbs4` 生成 VBS/VVC，最后清理临时文件。`resources/` 只存放 checked-in fixture；如果直接对 `resources/video/...` 运行 `python dev.py vtm analyze`，生成物会写到 `build/vtm_analysis/<视频名>/`。

```bash
python -m pytest native/analysis/tests/python/formats -v
```

## UI 回归测试

影响 Flutter 控制流、FFI action、主窗口交互、seek/上屏视觉结果时，native 测试不够，需要补跑 `dev.py ui-test`。

当前与 renderer 相关的重点脚本：

| 脚本 | 目的 |
|------|------|
| `ui_tests/h265_seek_visual_regression.csv` | HEVC 硬解 seek 后非黑帧且画面变化 |
| `ui_tests/h265_timeline_click_visual_regression.csv` | 通过真实 timeline pointer 点击触发 HEVC seek，验证非黑帧且画面变化 |
| `ui_tests/av1_not_black_regression.csv` | AV1 硬解 hwdownload 添加/seek 非黑帧 |
| `ui_tests/vp9_not_black_regression.csv` | VP9 硬解 hwdownload 添加/seek 非黑帧且 hash 变化 |

## 基准

可执行文件: `pipeline_bench.exe`，源码位于 `video_renderer/benchmarks/`。日常构建默认跳过 benchmarks；需要运行时使用：

```bash
python native/build.py --benchmarks-only
```

| 基准 | 测量内容 |
|------|---------|
| `bench_demux_only` | 解复用吞吐 |
| `bench_demux_decode` | 解复用 + 解码 |
| `bench_demux_decode_sws` | 软件转换到 RGBA |
| `bench_demux_decode_sws_d3d11` | RGBA 上传到 D3D11 |
| `bench_demux_decode_sws_d3d11_reuse` | 纹理复用上传 |
| `bench_full_pipeline` | 完整管线含 Present |

## Demo

`video_renderer/demo/demo_video_renderer.py` 是 PySide6 交互式 demo；`video_renderer/demo/demo_seek.py` 是 seek/逐帧的自动演示。日常播放器行为验证优先使用 `dev.py launch` 和 `dev.py ui-test`。
