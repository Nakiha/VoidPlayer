# AGENTS.md

## 项目概述

Windows 端 Flutter 播放器应用，使用 DX11 进行视频渲染，通过 Flutter Texture widget 显示。

### 技术栈

- **前端 UI**: Flutter (Dart)，Material Design 控件
- **窗体效果**: `flutter_acrylic` — Mica 背景效果，原生 Win32 窗口
- **主题适配**: 自动跟随 Windows 暗色/亮色主题，主题色从注册表读取 (`HKCU\Software\Microsoft\Windows\DWM\AccentColor`)
- **视频渲染**: C++ Native 模块，基于 FFmpeg + D3D11 的多轨道视频渲染器
- **纹理桥接**: 通过 Flutter Texture widget + FFI 将 DX11 纹理传递给 Flutter 显示，鼠标等输入事件通过 FFI 参数传递

### 目标平台

仅 Windows

## 开发脚本

一站式开发脚本 `dev.py`

```bash
# 构建
python dev.py build --native         # 仅构建 native C++ 模块

# 运行 Flutter
python dev.py launch                    # 运行 (release)
python dev.py launch --debug            # 运行 (debug，支持 hot reload)
python dev.py launch --log-level flutter=DEBUG,native=TRACE   # 传递日志级别

# Native Demo
python dev.py demo                   # 运行 PySide6 交互式 demo

# 测试
python dev.py ui-test ui_tests/smoke/basic.csv                # 启动 app 并执行 UI 自动化脚本
python dev.py test                   # Flutter 单元测试 + native 构建/测试
python dev.py test --native-only     # 仅 native 构建/测试
```

## Agent 工作约定

### Native C++ 构建 & Flutter 重编译

- **`python dev.py build --native` 只构建独立 native 模块**（Python pybind 扩展 + 静态库 + FFI DLL），**不会重新编译 Flutter Windows runner**。Flutter runner 通过自己的 CMake（`windows/runner/CMakeLists.txt`）把 `native/` 下的 C++ 源文件直接编译进 `void_player.exe`。
- **修改 `native/` 下的 C++ 代码后，必须执行 `flutter build windows --release`（或 `dev.py ui-test --build`）才能把变更编译进最终程序**。只跑 `dev.py build --native` 然后跑 UI 测试，测试的是旧代码。
- 验证流程：`dev.py build --native`（跑 native 单元测试）→ `flutter build windows --release`（编译进 Flutter exe）→ `dev.py ui-test`（UI 回归）。

### 验证优先级

- 修改 **Flutter UI / Action / 窗口交互 / 播放控制流程 / 主窗口 coordinator 生命周期** 后，必须优先使用 `ui_tests/` 下对应目录的 CSV 做一次自动化闭环验证，而不是只跑 Flutter 单元测试或只做静态阅读；`dev.py ui-test` 可以一次传多个 CSV 并顺序执行；添加 `--build` 可以顺带构建 flutter 程序
- 通用 smoke 首选命令：`python dev.py ui-test ui_tests/smoke/basic.csv`。如果本次改动涉及加载、播放、seek、layout、track、analysis 等具体路径，应在 smoke 之外追加对应目录脚本。
- `python dev.py test --flutter-only` 目前不能作为任何flutter侧修改测试，因为只验证了app link的拉起效果，并且作为重native重gpu交互的程序flutter单侧单元测试的实际效果非常有限

### UI 自动化选择

- `ui_tests/analysis/` 覆盖主窗体 spawn analysis 窗体、analysis 子窗体脚本、IPC track 更新；修改 `lib/windows/analysis/` 或 analysis 启动/IPC 流程时优先从这里选脚本，而不是只跑 smoke。
- `ui_tests/timeline/` 覆盖真实 timeline pointer/click 路径；修改 timeline / seek / 硬解上屏相关逻辑时，优先选这里的真实点击路径脚本，而不是只跑直接调用 native seek 的脚本。
- `ui_tests/seek/` 覆盖直接 seek / step / rapid seek；`ui_tests/loop/` 覆盖 loop range；`ui_tests/viewport/` 覆盖窗口尺寸、pan/zoom、split 布局；`ui_tests/track/` 覆盖轨道级修改；`ui_tests/codec/` 覆盖 codec 上屏 smoke；`ui_tests/local/` 是依赖个人绝对路径的非通用回归。
- 如果本次改动影响特定交互，应顺手新增或更新一条对应目录下的 `ui_tests/**/*.csv`，再执行它完成验证。
- 如果自动化脚本无法覆盖本次改动，需要在最终说明里明确写出阻塞点，以及还缺少哪个 Action / Assert / 启动参数。
- 修改 native C++ 模块时，仍应至少运行 `python dev.py test` 或 `python dev.py test --native-only`；如果改动同时影响主窗口交互，补跑一条 UI 脚本。

## 模块文档

- **Flutter / Dart 层** — UI 架构、主窗口 controller/coordinator、Action、UI 自动化 → [lib/doc.md](lib/doc.md)
- **Windows 宿主层** — Flutter runner、Win32 窗口宿主、Texture plugin / MethodChannel 桥接 → [windows/doc.md](windows/doc.md)
- **Native C++ 层** — C++ 视频渲染/解码/分析模块、架构、维护规范 → [native/docs/ARCHITECTURE.md](native/docs/ARCHITECTURE.md)
