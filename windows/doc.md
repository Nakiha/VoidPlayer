# Windows 平台文档

## 目录结构

- **`flutter/`** — Flutter 引擎集成层，由 Flutter 工具自动生成。包含插件注册、CMake 构建配置，**不应手动修改**
- **`runner/`** — Win32 应用宿主。负责创建原生窗口、初始化 Flutter 引擎、通过 MethodChannel 将 DX11 纹理桥接为 Flutter Texture（`video_renderer_plugin.h`）
- **`native/`** — C++ 视频渲染引擎，基于 FFmpeg + D3D11 的多轨道视频渲染器

## Native 模块文档入口

工程文档采用渐进式披露结构，入口文件：

- **[native/docs/ARCHITECTURE.md](native/docs/ARCHITECTURE.md)** — 总览 + 子文档索引

## 维护规范

修改 native 模块时遵循红绿灯 TDD：Red → Green → Refactor → **文档同步**。

详见 **[native/docs/MAINTENANCE.md](native/docs/MAINTENANCE.md)**。
