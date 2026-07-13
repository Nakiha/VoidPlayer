# Windows 宿主层

Windows 当前处于 native presentation 重建边界，不是可播放产品路径。

## 当前可用内容

- Win32 Flutter runner、窗口生命周期和插件注册；
- `video_renderer` MethodChannel/EventChannel 的 fail-closed 壳；
- 原生文件选择器；
- standalone native 模块中的 D3D11VA provider、独立 decode device 和稳定 shared snapshot frame storage；
- `native/windows/presentation/windows_presentation_backend.*` 中的新 backend factory 合同。

创建 player 或添加媒体会返回 `BACKEND_UNAVAILABLE`。runner 不链接 FFmpeg、D3D、
DComp 或 shared native renderer，也不要求 patched Flutter engine。D3D11VA 基础目前只在
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

旧 DComp compositor、source projection、FP16 ring、window capture 和 native player
bridge 已删除。需要算法参考时看历史提交，不要把旧文件复制回 active tree。

相关合同见 [Sandwich rendering](../native/docs/SANDWICH_RENDERING.md) 和
[Native architecture](../native/docs/ARCHITECTURE.md)。
