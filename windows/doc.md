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
- 默认解析 Windows Auto presentation：SDR-only 使用 BGRA8 native
  compositor，PQ/HLG + HDR output 使用 FP16 scRGB；output adapter 不匹配时
  通过 native compositor 迁移输出设备并使用 cross-adapter GPU-copy bridge，
  不能桥接时可诊断降级到 native SDR；保留 `sdr`、
  `native-compositor-sdr` 和 `native-compositor-scrgb` 强制诊断模式，
  禁止回到 Flutter Texture SDR 视频上屏
- 监听 display/settings/move/DPI 变化并刷新 output、SDR white level 与
  DComp target，不重建 player
- 在 D3D11/DComp/source-cache/transport device-loss 时通过 native
  presentation recovery 原地重建资源，保持 player、track、timeline 和最后
  成功帧；debug UI 自动化可用
  `debugSimulateWindowsDeviceLoss` / `DEBUG_SIMULATE_WINDOWS_DEVICE_LOSS`
  注入合成的 removed/reset/hung 场景
- 注册 native texture id 仅用于 controller/player lifecycle；Windows 视频
  上屏不得使用 Flutter Texture fallback
- 在 compositor opt-in 下消费 Flutter engine 导出的完整 alpha surface
  的 D3D12 shared texture / fence / generation，并把该 surface 交给
  wgpu/D3D12 render core 做最终合成；runner 只保留窗口、DComp/DXGI
  present、HDR/SDR target 和 device-loss 边界
- 通过既有 source-projection MethodChannel 校验 projection/signature，并让
  wgpu/D3D12 render core 对最多四轨 source-resolution bundle 实时执行
  pan/zoom/split；runner 不再通过 D3D11 source-cache bridge 消费 source
  bundle，也不再通过 D3D11 overlay bridge 合成标注
- retained D3D11 source/Flutter graph 不是产品路径；投影交互性能应通过
  wgpu source consume、Flutter surface consume、present cadence 和
  `windowsHotPath*` 诊断证明
- source-projection overlay 由 wgpu/D3D12 composite pass 消费 video-space
  primitives；新增 overlay 能力应继续落在 wgpu renderer，pan/zoom/split/order
  不能在每次 tick 重建 CPU vertices
- Flutter Windows runner 目标不再编译 `D3D11RenderBackend` / D3D11 overlay
  renderer；这些只保留在 standalone native parity/test 构建中。runner 仍可在
  DComp present bridge 内使用少量 D3D11 transport，直到 DX12 present target
  接管。
- 暴露 high-refresh interaction diagnostics，UI 自动化可用
  `RESET_NATIVE_PERF_COUNTERS`、`BEGIN_NATIVE_INTERACTION_SAMPLE` 和
  `END_NATIVE_INTERACTION_SAMPLE` 包住 pan/zoom/split/overlay 采样窗口
- 暴露 `windowsHotPath*` 汇总诊断，作为 source-projection / retained
  overlay 热路径的首选排查入口；低刷显示器给功能证据，高刷显示器必须让
  hot-path gate 通过
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
│   ├── windows_native_compositor.* # DComp/DXGI present bridge + migration compatibility
│   └── video_renderer_plugin.*    # video_renderer MethodChannel + Texture bridge
└── libs/ffmpeg/                   # Windows FFmpeg DLL bundle / import libs
```

## 边界规则

- `flutter/` 目录由 Flutter 工具生成，除非升级 embedding 或修复生成层问题，否则不要手改。
- `runner/` 可以处理 Win32 窗口、插件注册、MethodChannel 参数和 Texture bridge。
- `runner/analysis_ffi.*` 可以做 Dart FFI 参数校验、cache path/publish、工具进程调度和 native analysis handle 管理；具体 VAC2/VACHUNK 格式仍归 `native/analysis`。
- 复杂渲染/解码/同步逻辑不要写进 `runner/`，应放在 `native/`。
- runner 只解析 Windows presentation 请求和 display capability；FP16
  target、颜色映射、Flutter UI 合成和 fallback 应收敛在
  `PresentationBackend` / wgpu D3D12 backend。
- source cache 纹理创建、384 MiB budget、bundle generation/lease 和 source
  pass 由 wgpu D3D12 backend 承载；runner 只校验 wire 参数、维护 signature，
  并透出迁移期兼容诊断。
- source-projection 的 `currentPresentedFrame` anchor 由 renderer 在完整
  source-cache bundle 发布成功后更新；runner 只透出
  `nativeCompositorPresentedAnchor*` diagnostics，不从 compositor 消费状态反推帧。
- cross-adapter transport 属于 Windows presentation/native compositor 边界；
  runner 只传递 producer/output adapter、刷新 display capability，并发布诊断。
  禁止用 CPU readback、窗口截图或私有 ICC/LUT 替代 GPU-copy bridge 和系统
  Advanced Color 校准。`VOIDPLAYER_WINDOWS_CROSS_ADAPTER_SYNC=shared-fence`
  只用于本地多 adapter A/B 证据；默认仍是 event-query，shared-fence 失败必须
  可诊断回落 event-query。
- device-loss recovery 属于 `PresentationBackend`、wgpu/D3D12 backend 和
  `WindowsNativeCompositor` 边界；runner 只暴露 debug 注入、ACK/serial 和
  diagnostics。恢复失败按 native scRGB -> native SDR -> fail closed
  的可诊断顺序处理，不销毁 player 或 track model，不恢复 Flutter Texture 视频。
- 默认 Auto 与所有 native-compositor 模式必须使用锁定的 VoidPlayer
  Flutter local engine；
  普通 Flutter SDK 缺少 surface-export ABI，启动时必须 fail closed。
- active native compositor 必须通过 locked engine 的 compositor-owned
  surface export stream 接收 Flutter UI 更新；runner 不得在 active
  状态切回 `mirror`、恢复普通 HWND present，或用 Flutter Texture 视频作为成功
  fallback。缺少 surface-export ABI 或 requested export generation 超时必须显式
  fail closed。
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
