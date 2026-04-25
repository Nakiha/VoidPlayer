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
python dev.py ui-test ui_tests/smoke_basic.csv                # 启动 app 并执行 UI 自动化脚本

# Native Demo
python dev.py demo                   # 运行 PySide6 交互式 demo

# 测试
python dev.py test                   # 构建 + 测试 native 模块
```

## Agent 工作约定

- 修改 **Flutter UI / Action / 窗口交互 / 播放控制流程** 后，优先使用 `ui_tests/*.csv` 做一次自动化闭环验证，而不是只做静态阅读。
- 首选命令：`python dev.py ui-test ui_tests/smoke_basic.csv`
- 修改 timeline / seek / 硬解上屏相关逻辑时，优先补跑更贴近真实点击路径的 `python dev.py ui-test ui_tests/h265_timeline_click_visual_regression.csv`，而不是只跑直接调用 native seek 的脚本。
- 如果本次改动影响特定交互，应顺手新增或更新一条对应的 `ui_tests/*.csv`，再执行它完成验证。
- 如果自动化脚本无法覆盖本次改动，需要在最终说明里明确写出阻塞点，以及还缺少哪个 Action / Assert / 启动参数。
- 修改 native C++ 模块时，仍应至少运行 `python dev.py test`；如果改动同时影响主窗口交互，补跑一条 UI 脚本。

## 模块文档

- **Flutter 侧** — 日志系统、交互系统 (Action) 等 → [lib/doc.md](lib/doc.md)
- **Native 侧** — C++ 视频渲染引擎、架构、维护规范 → [windows/doc.md](windows/doc.md)
