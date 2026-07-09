# Windows 宿主层文档

本文档是 Windows Flutter runner / Win32 宿主层入口。macOS runner 文档见
[../macos/doc.md](../macos/doc.md)，shared native renderer 文档见
[../native/docs/ARCHITECTURE.md](../native/docs/ARCHITECTURE.md)。

## 当前状态

本 restart 分支以 macOS native Metal 为主线。Windows native presentation
后端已经从 active build/test/gate 路径移除，`native-d3d11` 和
`native-d3d12` 只保留为后续 runner-composed sandwich backend 的接口占位。

当前 Windows runner 可以继续承载：

- Win32 主窗口、Flutter engine 初始化和插件注册；
- `video_renderer` MethodChannel / EventChannel 壳层；
- 文件选择、窗口捕获、日志、analysis FFI、诊断转发等平台服务；
- native player lifecycle 的 fail-closed 桥接。

当前 Windows runner 不应声明：

- DComp/D3D11/D3D12 视频上屏产品路径可用；
- Windows HDR / high-refresh / source-projection / cross-adapter gate 可用；
- Flutter Texture SDR 是正常视频 fallback。

## 目录结构

```text
windows/
├── CMakeLists.txt                 # Windows runner/native/plugin 构建入口
├── flutter/                       # Flutter 工具生成的 embedding 集成层
└── runner/                        # Win32 应用宿主和插件桥接代码
    ├── analysis_ffi.*             # analysis FFI bridge
    ├── flutter_window.*           # Flutter window / plugin 注册
    ├── main.cpp                   # Windows app 入口
    ├── native_*                   # native lifecycle / diagnostics bridge
    ├── video_renderer_plugin.*    # video_renderer MethodChannel 壳层
    └── win32_window.*             # Win32 窗口包装
```

## 边界规则

- `flutter/` 目录由 Flutter 工具生成，除非升级 embedding 或修复生成层问题，否则不要手改。
- `runner/` 可以处理 Win32 窗口、插件注册、MethodChannel 参数、平台服务和诊断。
- 复杂渲染、解码、同步和 presentation backend 逻辑应放在 `native/`。
- Windows native presentation 重新接入时，必须按
  [../native/docs/SANDWICH_RENDERING.md](../native/docs/SANDWICH_RENDERING.md)
  建立新的 D3D11/DX12 backend 和验证矩阵，而不是复活旧 DComp/source-projection
  preservation profiles。

## 相关文档

| 文档 | 内容 |
|------|------|
| [../lib/doc.md](../lib/doc.md) | Flutter / Dart UI 层入口 |
| [../native/docs/ARCHITECTURE.md](../native/docs/ARCHITECTURE.md) | Native C++ 渲染引擎入口 |
| [../native/docs/SANDWICH_RENDERING.md](../native/docs/SANDWICH_RENDERING.md) | runner-composed native sandwich 合同 |
| [../native/docs/NATIVE_EVENT_PIPELINE.md](../native/docs/NATIVE_EVENT_PIPELINE.md) | native -> Dart EventChannel 事件通知合同 |
| [../native/docs/FFI_AND_BINDINGS.md](../native/docs/FFI_AND_BINDINGS.md) | Native FFI / Python 绑定说明 |
| [../native/docs/MAINTENANCE.md](../native/docs/MAINTENANCE.md) | Native 层维护规范 |
