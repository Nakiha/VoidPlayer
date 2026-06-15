# Windows 宿主层文档

本文档是 Windows Flutter runner / Win32 宿主层入口。macOS runner 文档见
[../macos/doc.md](../macos/doc.md)，shared native renderer 文档见
[../native/docs/ARCHITECTURE.md](../native/docs/ARCHITECTURE.md)。

## 模块定位

`windows/` 是 Flutter Windows embedding 和本项目 Win32 宿主集成层，负责：

- 创建和管理原生 Win32 主窗口
- 初始化 Flutter engine 和插件注册
- 提供 `video_renderer` MethodChannel / Texture plugin 桥接
- 提供 `video_renderer/events` EventChannel 和 native 诊断桥接
- 按主窗口与 DXGI output 的最大交集探测当前显示器、color space 和亮度元数据
- 解析 `VOIDPLAYER_WINDOWS_PRESENTATION_MODE=fp16-scrgb` 或
  `native-compositor-scrgb`，并在创建 player
  时锁定当前 output 的 SDR white level
- 将 native DX11 shared texture 暴露给 Flutter Texture widget
- 在 compositor opt-in 下消费 Flutter engine 导出的完整 alpha surface，
  与共享 FP16 video ring 合成到同一 DComp swap chain
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
│   ├── analysis_ffi.*             # VAC2/VACHUNK generation, cache publish, overlay state FFI
│   ├── main.cpp                   # Windows app 入口
│   ├── win32_window.*             # Win32 窗口包装
│   ├── windows_native_compositor.* # Flutter surface + FP16 video 的 DComp final composite
│   └── video_renderer_plugin.*    # video_renderer MethodChannel + Texture bridge
└── libs/ffmpeg/                   # Windows FFmpeg DLL bundle / import libs
```

## 边界规则

- `flutter/` 目录由 Flutter 工具生成，除非升级 embedding 或修复生成层问题，否则不要手改。
- `runner/` 可以处理 Win32 窗口、插件注册、MethodChannel 参数和 Texture bridge。
- `runner/analysis_ffi.*` 可以做 Dart FFI 参数校验、cache path/publish、工具进程调度和 native analysis handle 管理；具体 VAC2/VACHUNK 格式仍归 `native/analysis`。
- 复杂渲染/解码/同步逻辑不要写进 `runner/`，应放在 `native/`。
- runner 只解析 Windows presentation 请求和 display capability；FP16
  target、颜色映射和 fallback 实现在 `PresentationBackend` / D3D11 backend。
- `native-compositor-scrgb` 必须使用锁定的 VoidPlayer Flutter local engine；
  普通 Flutter SDK 缺少 surface-export ABI，启动时只能诊断性回落。
- 禁止 color-key、`WS_EX_LAYERED`、窗口截图、桌面捕获和 child HWND sandwich。
- 本地 engine 依次使用 `scripts/ci/build_flutter_windows_engine.ps1`、
  `package_flutter_windows_engine.ps1` 和
  `bootstrap_flutter_windows_engine.ps1` 构建、校验并安装；DComp runner
  不能混用普通 Flutter SDK 产物。
- Flutter UI 行为不要写进 `runner/`，应放在 `lib/`。
- FFmpeg Windows bundle 的文件位置可以在这里记录，但 FFmpeg/native 管线设计仍归 native 文档维护。

## 相关文档

| 文档 | 内容 |
|------|------|
| [../lib/doc.md](../lib/doc.md) | Flutter / Dart UI 层入口 |
| [../native/docs/ARCHITECTURE.md](../native/docs/ARCHITECTURE.md) | Native C++ 渲染引擎入口 |
| [../native/docs/WINDOWS_PRESENTATION_BACKEND.md](../native/docs/WINDOWS_PRESENTATION_BACKEND.md) | Windows 产品上屏合同、诊断和追赶路线 |
| [../native/docs/D3D11_BACKEND.md](../native/docs/D3D11_BACKEND.md) | D3D11 device/texture/shader 实现细节 |
| [../native/docs/NATIVE_EVENT_PIPELINE.md](../native/docs/NATIVE_EVENT_PIPELINE.md) | native -> Dart EventChannel 事件通知合同 |
| [../native/docs/FFI_AND_BINDINGS.md](../native/docs/FFI_AND_BINDINGS.md) | Native FFI / Python 绑定说明 |
| [../native/docs/MAINTENANCE.md](../native/docs/MAINTENANCE.md) | Native 层维护规范 |
