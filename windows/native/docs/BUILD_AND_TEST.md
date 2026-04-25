# 构建与测试

## 入口命令

项目根目录下优先使用 `dev.py`，它会串起 native 构建、CTest 和 Flutter 侧需要的产物复制。

```bash
python dev.py build --native
python dev.py test
python dev.py ui-test test_scripts/smoke_basic.csv
```

Native 子目录也可以单独运行：

```bash
python windows/native/build.py
python windows/native/build.py --build-only
python windows/native/build.py --test-only
python windows/native/build.py --benchmarks-only
python windows/native/build.py --debug
```

如果 FFmpeg 不在默认的 `windows/libs/ffmpeg`，可以显式指定：

```bash
python windows/native/build.py --ffmpeg-root <ffmpeg-root>
```

PowerShell 中也可以用环境变量：

```powershell
$env:FFMPEG_ROOT = "<ffmpeg-root>"
python windows/native/build.py
```

## CMake 目标

| 目标 | 类型 | 说明 |
|------|------|------|
| `video_renderer_lib` | STATIC | 核心渲染/解码/同步管线 |
| `video_renderer_ffi` | SHARED | C FFI DLL，导出 `naki_vr_*` |
| `video_renderer_native` | MODULE | Python 扩展 `.pyd`，供 demo/脚本调用 |
| `video_renderer_tests` | EXE | Catch2 renderer 单元/集成测试 |
| `analysis_tests` | EXE | H.266 分析数据、VBI/VBT 解析与生成测试 |
| `test_ffi_c` | EXE | C ABI smoke test |
| `probe_hw` | EXE | 硬件能力探测，存在 `probe_hw.cpp` 时构建 |
| `pipeline_bench` | EXE | 解复用/解码/上传/Present 基准，`BUILD_BENCHMARKS=ON` 时构建 |

## 依赖

| 依赖 | 来源 | 用途 |
|------|------|------|
| FFmpeg | `windows/libs/ffmpeg` | demux、软解、D3D11VA 硬解、hwdownload |
| spdlog | 本地 `_deps` 优先，缺失时 FetchContent | native 日志 |
| Catch2 | 本地 `_deps` 优先，缺失时 FetchContent | C++ 测试 |
| pybind11 | `find_package` | Python 绑定 |
| VTM DecoderApp | `windows/native/tools/vtm` | analysis 测试生成 VBS2 |

## `python dev.py test` 实际覆盖

`dev.py test` 会先构建 native Release，再执行 `windows/native/build.py --test-only`，当前 CTest 包含 3 个测试目标。

| CTest | 覆盖 |
|------|------|
| `video_renderer_tests` | Clock、PacketQueue、TrackBuffer、DemuxThread、DecodeThread、FrameConverter、D3D11 device/texture/shader、RenderSink、Renderer integration，并包含 headless front-buffer capture 的 HEVC/AV1/VP9 视觉回归 |
| `analysis_tests` | H.266 分析模块，VBI/VBT 生成与解析，测试数据生成/清理 |
| `test_ffi_c` | 未初始化 renderer、空指针、基础 lifecycle、C ABI 可调用性 |

测试视频默认来自 `resources/video`，CMake 通过 `VIDEO_TEST_DIR` 注入。

## UI 回归测试

影响 Flutter 控制流、FFI action、主窗口交互、seek/上屏视觉结果时，native 测试不够，需要补跑 `dev.py ui-test`。

当前与 renderer 相关的重点脚本：

| 脚本 | 目的 |
|------|------|
| `test_scripts/h265_seek_visual_regression.csv` | HEVC 硬解 seek 后非黑帧且画面变化 |
| `test_scripts/h265_timeline_click_visual_regression.csv` | 通过真实 timeline pointer 点击触发 HEVC seek，验证非黑帧且画面变化 |
| `test_scripts/av1_not_black_regression.csv` | AV1 硬解 hwdownload 添加/seek 非黑帧 |
| `test_scripts/vp9_not_black_regression.csv` | VP9 硬解 hwdownload 添加/seek 非黑帧且 hash 变化 |

## 基准

可执行文件: `pipeline_bench.exe`。日常构建默认跳过 benchmarks；需要运行时使用：

```bash
python windows/native/build.py --benchmarks-only
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

`demo_video_renderer.py` 是 PySide6 交互式 demo；`demo_seek.py` 是 seek/逐帧的自动演示。日常播放器行为验证优先使用 `dev.py launch` 和 `dev.py ui-test`。
