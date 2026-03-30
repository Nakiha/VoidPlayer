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

仅 Windows（已移除 android、ios、linux、macos、web 目录）

## Native 模块 (C++ 视频渲染引擎)

源码位于 `windows/native/`，基于 FFmpeg + D3D11 的多轨道视频渲染器。

### 文档入口

工程文档采用渐进式披露结构，入口文件：

- **[windows/native/docs/ARCHITECTURE.md](windows/native/docs/ARCHITECTURE.md)** — 总览 + 子文档索引

### 维护规范

修改 native 模块时遵循红绿灯 TDD：Red → Green → Refactor → **文档同步**。

详见 **[windows/native/docs/MAINTENANCE.md](windows/native/docs/MAINTENANCE.md)**。
