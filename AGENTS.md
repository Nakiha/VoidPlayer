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

# Agent 协议(常驻控制通道,见 lib/docs/AGENT_PROTOCOL.md)
python dev.py agent-smoke
python dev.py agent session --connection-file <path>

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
- Windows presentation 改动必须经过 `PresentationBackend` / D3D11 backend 边界，不在 shared scheduler 或 runner 中复制渲染策略。
- Windows presentation fallback 必须显式、可诊断；改变 adapter、driver type、target format 或 presentation mode 时必须同步日志和 diagnostics。
- 修改 shared presentation backend、Windows texture/format/color 路径时，必须运行确定性 D3D11 color/layout parity 与 FP16/scRGB smoke，以及 `windows-preservation`。
- Windows native compositor 只能合成锁定 Flutter engine 导出的完整 premultiplied-alpha surface；禁止 color-key、`WS_EX_LAYERED`、窗口/桌面捕获、矩形 hard hole 或 child HWND sandwich。
- 修改 Flutter surface export、共享 FP16 ring、DComp 激活/回落协议时，必须使用锁定 Windows local engine 跑 DComp native canary 和 rebuilt UI smoke；普通 Flutter SDK 的 fallback 不能作为通过证据。
- 修改 Windows source cache/projection、bundle lease、DComp projection shader 或 native overlay 合成时，必须运行 `[windows_source_cache]`、`[windows_source_projection]`、`windows_d3d11_source_projection_smoke` 和 rebuilt source-projection UI smoke，并保持 viewport FP16 fallback 可诊断。
- 修改 Windows Auto policy、display refresh、SDR/scRGB DComp target 或 SDR white-level 更新时，必须同时验证默认 Auto SDR 与强制 scRGB；HDR target 失败必须先降级 native SDR，不能直接恢复 Flutter Texture。具备 HDR 显示时补跑 `python dev.py gate windows-hdr-auto`。
- 不要在一个轮次里堆无关改动。每轮完成后先测试，再单独提交。

## 验证矩阵

按本轮改动的影响面选择验证命令。多个影响面叠加时取并集。

| 改动类型 | 必跑验证 |
| --- | --- |
| native C++ 单元逻辑 | `python dev.py gate pr-fast` 或 `python dev.py test --native-only` |
| native C++ 影响 Windows runner / Texture / 渲染上屏 | `python dev.py gate windows-preservation` 或等价 Windows build + UI smoke |
| native C++ 影响 macOS runner / Texture / Metal 上屏 | `python dev.py gate macos-ui-smoke` 或相关 `python dev.py mac-ui-test --build ...` |
| shared renderer / presentation backend 边界 | 对应平台 backend parity smoke + macOS 相关 smoke + Windows preservation gate |
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

## 新增测试时机

- 改动纯 Dart 状态机、数据模型、Action 分发、持久化序列化、时间/帧锚定、列表过滤排序或 coordinator 决策时，优先新增或更新 `test/unit/` 中的 Flutter/Dart 测试，用最小输入锁住边界条件；不要只依赖上屏 UI 自动化。
- 改动 Flutter widget 的布局、命中测试、焦点、快捷键、悬浮态、编辑态或 view model 到 widget 的绑定时，优先补对应 widget/unit 测试；只有真实平台事件、native texture、窗口尺寸或输入设备路径无法在 widget 测试覆盖时，再补 `ui_tests/**/*.csv`。
- 改动 native C++ 的可独立计算逻辑、seek/clock/layout/presentation decision、cache/index/parser 或格式转换边界时，优先补 native 单元测试，并运行 `python dev.py test --native-only` 或覆盖该模块的 gate。
- 修复回归 bug 时，先在最低层可复现该 bug 的测试层级加回归用例：Dart 单测能复现就不用升到 UI 脚本；只有跨 Flutter/native 上屏同步、runner 集成或真实输入序列才升到 UI 自动化。

## UI 自动化选择

- UI 自动化是最高成本验证层。新增 CSV 前先判断能否用 Dart/widget/native 单测覆盖；能下沉就下沉，不能下沉时优先更新既有同目录脚本，而不是新增相邻场景脚本。
- `ui_tests/analysis/` 覆盖主窗体 spawn analysis 窗体、analysis 子窗体脚本、IPC track 更新；修改 `lib/windows/analysis/`、analysis 启动/IPC 流程、analysis toolbar 入口或 analysis cache/overlay 交互时，优先从这里选脚本，而不是只跑 smoke。
- `ui_tests/timeline/` 覆盖真实 timeline pointer/click 路径；修改 timeline / seek / 硬解上屏相关逻辑时，优先选这里的真实点击路径脚本，而不是只跑直接调用 native seek 的脚本。
- `ui_tests/seek/` 覆盖直接 seek / step / rapid seek；`ui_tests/loop/` 覆盖 loop range；`ui_tests/viewport/` 覆盖窗口尺寸、pan/zoom、split 布局；`ui_tests/track/` 覆盖轨道级修改；`ui_tests/codec/` 覆盖 codec 上屏 smoke；`ui_tests/local/` 是依赖个人绝对路径的非通用回归。
- `ui_tests/macos/` 覆盖 macOS runner、FlutterTexture/CVPixelBuffer、Metal presentation、VideoToolbox/software fallback、layout/seek/audio/callback 生命周期；macOS 改动优先从这里选脚本。
- 只有本次改动依赖真实上屏、native texture、runner 集成、窗口尺寸、平台输入序列、跨 Flutter/native 时序或 codec/hardware 后端时，才新增或更新 `ui_tests/**/*.csv`；纯 Flutter 状态、列表筛选、view model、coordinator 分支、数据落盘等回归应优先落到 `test/unit/` 或 native 单测。
- 新增 UI CSV 时要说明它覆盖的独有风险；如果只是已有 smoke/regression 的参数变体，优先扩展原脚本或补单测。stress/resource/visual/hash 类脚本不要放入默认验证路径，除非本轮改动正好触碰对应风险。
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
- **macOS 发布准备**: [native/docs/MACOS_READINESS.md](native/docs/MACOS_READINESS.md)
- **macOS Presentation Backend**: [native/docs/MACOS_PRESENTATION_BACKEND.md](native/docs/MACOS_PRESENTATION_BACKEND.md)
- **Native 构建与测试**: [native/docs/BUILD_AND_TEST.md](native/docs/BUILD_AND_TEST.md)
