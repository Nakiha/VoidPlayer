# Windows 宿主层

Windows 当前处于 native presentation 重建边界，不是可播放产品路径。

## 当前可用内容

- Win32 Flutter runner、窗口生命周期和插件注册；
- `video_renderer` MethodChannel/EventChannel 的 fail-closed player 壳；
- 顶层 HWND 上 input-transparent 的 DComp final visual，已消费锁定 Flutter V1 D3D11 keyed-mutex lease；
- 原生文件选择器；
- standalone native 模块中的 D3D11VA provider、独立 decode device 和稳定 shared snapshot frame storage；
- runner-owned D3D11 complete-viewport target ring 状态机；
- `native/windows/presentation/windows_presentation_backend.*` 中已激活的 D3D11 target lifecycle backend；
- `windows_d3d11_viewport_renderer.*` 中已验证的 D3D11VA/CPU YUV/BGRA、shared layout 与 SDR/scRGB viewport shader。

创建 player 或添加媒体仍会返回 `BACKEND_UNAVAILABLE`。runner 已链接 D3D11/DComp 但尚未链接 FFmpeg
或 shared native renderer，并且强制要求 patched Flutter local engine。D3D11VA 与 viewport backend 目前只在
standalone native build/test 中生效；不要把 Flutter Texture
当作视频 fallback。

## 重建入口

后续 Windows 工作只从以下边界开始：

```text
Win32 window
  + native SDR/HDR video surface
  + Flutter premultiplied-ARGB surface
  -> runner composition
```

1. 在 `windows_presentation_backend.*` 实现 D3D11 或 D3D12 backend。
2. runner 只负责窗口、surface 绑定和最终合成，不接管 Flutter frame 调度。
   与 macOS `MacOSNativeCompositorView` 同构：最终 compositor 不参与 hit-test，
   不拦截 pointer/keyboard/gesture，不调用 Flutter frame request/pump；它只采样
   Flutter 自己已经发布的最新 premultiplied-alpha surface。
3. shared renderer 只提交 `RendererDrawSnapshot`，不暴露 GPU device、shared ring 或
   external-target draw 旁路。
4. 新后端必须建立自己的 color、layout、HDR、device-loss 和 UI smoke 矩阵。

当前选定的首条产品链路是 D3D11VA + D3D11。runner 分配 BGRA8/RGBA16F
target ring，native backend 只绘制完整 viewport；runner 使用 Flutter engine
现有 V1 D3D11 keyed-mutex lease 合成 UI。V2 的 D3D12 标签在 fork 提供真实
D3D12 resource/fence 之前不接入。

旧 DComp compositor、source projection、FP16 ring、window capture 和 native player
bridge 已删除。需要算法参考时看历史提交，不要把旧文件复制回 active tree。

相关合同见 [Sandwich rendering](../native/docs/SANDWICH_RENDERING.md) 和
[Native architecture](../native/docs/ARCHITECTURE.md)。
