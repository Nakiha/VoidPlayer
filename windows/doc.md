# Windows 宿主层文档

本文档是 Windows Flutter runner / Win32 宿主层入口。Native C++ 渲染引擎文档见
[../native/docs/ARCHITECTURE.md](../native/docs/ARCHITECTURE.md)。

## 模块定位

`windows/` 是 Flutter Windows embedding 和本项目 Win32 宿主集成层，负责：

- 创建和管理原生 Win32 主窗口
- 初始化 Flutter engine 和插件注册
- 提供 `video_renderer` MethodChannel / Texture plugin 桥接
- 将 native DX11 shared texture 暴露给 Flutter Texture widget
- 引入 native C++ renderer 构建产物和 Windows 运行时依赖

它不负责：

- Flutter UI 状态和交互业务；这些在 [../lib/doc.md](../lib/doc.md)
- C++ 解码、同步、D3D11 渲染器内部架构；这些在
  [../native/docs/ARCHITECTURE.md](../native/docs/ARCHITECTURE.md)

## 目录结构

```text
windows/
├── CMakeLists.txt                 # Windows runner/native/plugin 构建入口
├── flutter/                       # Flutter 工具生成的 embedding 集成层，通常不手改
├── runner/                        # Win32 应用宿主和插件桥接代码
│   ├── flutter_window.*           # Flutter window / plugin 注册
│   ├── main.cpp                   # Windows app 入口
│   ├── win32_window.*             # Win32 窗口包装
│   └── video_renderer_plugin.*    # video_renderer MethodChannel + Texture bridge
└── libs/ffmpeg/                   # Windows FFmpeg DLL bundle / import libs
```

## 边界规则

- `flutter/` 目录由 Flutter 工具生成，除非升级 embedding 或修复生成层问题，否则不要手改。
- `runner/` 可以处理 Win32 窗口、插件注册、MethodChannel 参数和 Texture bridge。
- 复杂渲染/解码/同步逻辑不要写进 `runner/`，应放在 `native/`。
- Flutter UI 行为不要写进 `runner/`，应放在 `lib/`。
- FFmpeg Windows bundle 的文件位置可以在这里记录，但 FFmpeg/native 管线设计仍归 native 文档维护。

## 相关文档

| 文档 | 内容 |
|------|------|
| [../lib/doc.md](../lib/doc.md) | Flutter / Dart UI 层入口 |
| [../native/docs/ARCHITECTURE.md](../native/docs/ARCHITECTURE.md) | Native C++ 渲染引擎入口 |
| [../native/docs/FFI_AND_BINDINGS.md](../native/docs/FFI_AND_BINDINGS.md) | Native FFI / Python 绑定说明 |
| [../native/docs/MAINTENANCE.md](../native/docs/MAINTENANCE.md) | Native 层维护规范 |
