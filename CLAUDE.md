# CLAUDE.md

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

一站式开发脚本 `dev.py`，替代 `build.py` + `run.py`：

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
python dev.py test                   # 构建 + 测试 native 模块
```

## 模块文档

- **Flutter 侧** — 日志系统、交互系统 (Action) 等 → [lib/doc.md](lib/doc.md)
- **Native 侧** — C++ 视频渲染引擎、架构、维护规范 → [windows/doc.md](windows/doc.md)