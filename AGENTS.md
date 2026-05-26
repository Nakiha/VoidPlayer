# AGENTS.md

## 项目概述

VoidPlayer 是 Flutter 桌面视频播放器。Flutter/Dart 负责跨平台 UI shell、播放控制、
Action/UI 自动化入口和平台服务注入；播放、解码、同步、布局和渲染调度由 shared
native C++ 模块负责。

- **Flutter / Dart UI**: 主窗口、播放控制、Action/UI 自动化入口、MethodChannel/EventChannel 编排。
- **Shared native renderer**: FFmpeg demux/decode、playback clock、seek/loop、track lifecycle、layout、RenderSink/PresentDecision、RendererDrawSnapshot。
- **Windows backend**: Win32 runner、D3D11/D3D11VA、shared texture / swap-chain presentation、Windows UI 自动化。
- **macOS backend**: Cocoa runner、sandbox file access、FlutterTexture、CVPixelBuffer lifecycle、Metal/CVPixelBuffer/IOSurface presentation、VideoToolbox 硬解。
- **Analysis**: native analysis/FFI/cache 工具共享；Windows analysis UI/IPC 可用，macOS analysis UI/IPC 仍 capability-gated。
- **当前平台状态**: Windows 是既有发布主路径；macOS native playback 已 feature-complete，处于 stabilization/release-readiness 阶段。

## 开发脚本

一站式开发脚本是 `dev.py`。

```bash
# 构建
python dev.py build --native

# 运行
python dev.py launch
python dev.py launch --debug
python dev.py launch --log-level flutter=DEBUG,native=TRACE

# Native demo
python dev.py demo

# 测试
python dev.py test
python dev.py test --native-only
python dev.py gate pr-fast
python dev.py gate macos-ui-smoke
python dev.py gate macos-release-readiness
python dev.py ui-test ui_tests/smoke/basic.csv
python dev.py ui-test --build ui_tests/smoke/basic.csv
python dev.py ui-test --build ui_tests/smoke/basic.csv ui_tests/viewport/viewport_pan_layout_regression.csv
```

## 硬约束

- `python dev.py build --native` 只构建独立 native 模块，不会重新编译 Flutter Windows runner。
- Flutter runner 会通过 `windows/runner/CMakeLists.txt` 直接编译 `native/` 下的 C++ 源文件进 `void_player.exe`。
- `python dev.py ui-test ...` 会运行 UI 自动化，但默认复用已有 Flutter Windows 产物；修改 Dart UI、Windows runner 或会编进 runner 的 `native/` C++ 后，必须执行 `python dev.py ui-test --build ...` 或先执行 `flutter build windows --release`，否则测试可能仍在跑旧程序。
- `python dev.py ui-test ...` 可以一次传入多个 CSV 脚本，`dev.py` 会在同一次构建/启动配置下串行执行这些用例。
- `python dev.py build --native` 不能替代 Flutter runner 重建；修改 `native/` C++ 后，必须执行 `flutter build windows --release` 或 `python dev.py ui-test --build ...`，否则上屏测试仍可能运行旧代码。
- macOS 上屏相关修改必须重建 macOS runner 或使用 `python dev.py mac-ui-test --build ...`，否则可能仍在跑旧 `.app`。
- native 渲染路径不引入 `libswscale` / `libyuv` 作为通用 fallback；新增像素格式支持时应做确定性转换，并验证软解/硬解颜色一致性。
- 不要在一个轮次里堆无关改动。每轮完成后先测试，再单独提交。

## 验证矩阵

按本轮改动的影响面选择验证命令。多个影响面叠加时取并集。

| 改动类型 | 必跑验证 |
| --- | --- |
| native C++ 单元逻辑 | `python dev.py gate pr-fast` 或 `python dev.py test --native-only` |
| native C++ 影响 Windows runner / Texture / 渲染上屏 | `python dev.py gate windows-preservation` 或等价 Windows build + UI smoke |
| native C++ 影响 macOS runner / Texture / Metal 上屏 | `python dev.py gate macos-ui-smoke` 或相关 `python dev.py mac-ui-test --build ...` |
| shared renderer / presentation backend 边界 | macOS 相关 smoke + 后续 Windows preservation gate |
| macOS package / signing / FFmpeg dylib / release docs | `python dev.py gate macos-release-readiness` |
| Flutter UI / Action / 主窗口 coordinator / 播放控制 | 相关 `python dev.py ui-test --build ...`，不要只跑 `python dev.py test --flutter-only` |
| 窗口、布局、pan/zoom、split | `ui_tests/viewport/` 中相关脚本，加 smoke |
| timeline 点击、seek、step、loop | `ui_tests/timeline/` / `ui_tests/seek/` / `ui_tests/loop/` 中相关脚本 |
| track 修改、codec、分析窗口/IPC | `ui_tests/track/` / `ui_tests/codec/` / `ui_tests/analysis/` 中相关脚本 |

通用 smoke 首选：

```bash
python dev.py ui-test --build ui_tests/smoke/basic.csv
```

macOS smoke 首选按影响面选择 `ui_tests/macos/` 中脚本，例如：

```bash
python dev.py mac-ui-test --build ui_tests/macos/native_facade_smoke.csv
python dev.py gate macos-ui-smoke
```

如果自动化脚本无法覆盖本轮风险，需要在最终说明里写清楚缺口，例如缺少哪个 Action、Assert 或启动参数。
多个 UI 影响面叠加时，优先把相关 CSV 放在同一条 `python dev.py ui-test --build ...` 命令中串行执行，避免重复构建和手动遗漏。

## UI 自动化选择

- `ui_tests/analysis/` 覆盖主窗体 spawn analysis 窗体、analysis 子窗体脚本、IPC track 更新；修改 `lib/windows/analysis/`、analysis 启动/IPC 流程、analysis toolbar 入口或 analysis cache/overlay 交互时，优先从这里选脚本，而不是只跑 smoke。
- `ui_tests/timeline/` 覆盖真实 timeline pointer/click 路径；修改 timeline / seek / 硬解上屏相关逻辑时，优先选这里的真实点击路径脚本，而不是只跑直接调用 native seek 的脚本。
- `ui_tests/seek/` 覆盖直接 seek / step / rapid seek；`ui_tests/loop/` 覆盖 loop range；`ui_tests/viewport/` 覆盖窗口尺寸、pan/zoom、split 布局；`ui_tests/track/` 覆盖轨道级修改；`ui_tests/codec/` 覆盖 codec 上屏 smoke；`ui_tests/local/` 是依赖个人绝对路径的非通用回归。
- `ui_tests/macos/` 覆盖 macOS runner、FlutterTexture/CVPixelBuffer、Metal presentation、VideoToolbox/software fallback、layout/seek/audio/callback 生命周期；macOS 改动优先从这里选脚本。
- 如果本次改动影响特定交互，应顺手新增或更新一条对应目录下的 `ui_tests/**/*.csv`，再用 `python dev.py ui-test --build ...` 执行它完成验证。
- 如果自动化脚本无法覆盖本次改动，需要在最终说明里明确写出阻塞点，以及还缺少哪个 Action / Assert / 启动参数。
- 修改 native C++ 模块时，仍应至少运行 `python dev.py test` 或 `python dev.py test --native-only`；如果改动同时影响主窗口交互，补跑一条带 `--build` 的 UI 脚本。

## 日志排查

`dev.py ui-test` 默认把日志写到 `%APPDATA%\VoidPlayer\logs`。

- Flutter/Dart 日志形如 `void_player_main_<pid>_<date>.log`
- native C++ 日志形如 `native_main_<pid>.log`
- UI 自动化失败后，先看最近一对 Flutter/native 日志；Flutter 日志通常包含 `TestRunner FAIL`，native 日志通常包含 demux/decode/render/seek 细节。

常用 PowerShell：

```powershell
Get-ChildItem "$env:APPDATA\VoidPlayer\logs" -File |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 8 FullName,Length,LastWriteTime
```

## 模块文档

- **Flutter / Dart 层**: [lib/doc.md](lib/doc.md)
- **Windows 宿主层**: [windows/doc.md](windows/doc.md)
- **macOS Runner 层**: [macos/doc.md](macos/doc.md)
- **Native C++ 层**: [native/docs/ARCHITECTURE.md](native/docs/ARCHITECTURE.md)
- **Native 线程模型**: [native/docs/THREADING_MODEL.md](native/docs/THREADING_MODEL.md)
- **Native 解码管线**: [native/docs/DECODE_PIPELINE.md](native/docs/DECODE_PIPELINE.md)
- **Native 色彩管线**: [native/docs/COLOR_PIPELINE.md](native/docs/COLOR_PIPELINE.md)
- **Native 平台后端计划**: [native/docs/RENDERER_PLATFORM_BACKEND_PLAN.md](native/docs/RENDERER_PLATFORM_BACKEND_PLAN.md)
- **macOS 移植/发布准备**: [native/docs/MACOS_PORT_PLAN.md](native/docs/MACOS_PORT_PLAN.md)
- **macOS Presentation Backend**: [native/docs/MACOS_PRESENTATION_BACKEND.md](native/docs/MACOS_PRESENTATION_BACKEND.md)
- **Native 构建与测试**: [native/docs/BUILD_AND_TEST.md](native/docs/BUILD_AND_TEST.md)
