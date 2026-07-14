# Windows 宿主层

Windows 已恢复可交互的 native SDR 播放路径。视频不经过 Flutter Texture；
runner 使用 DComp 合成 shared renderer 输出的 D3D11 视频 target 与 Flutter engine
导出的完整 premultiplied-alpha UI surface。

## 当前可用内容

- Win32 Flutter runner、窗口生命周期和插件注册；
- `video_renderer` MethodChannel/EventChannel 的 Windows native player facade；
- 顶层 HWND 上 input-transparent 的 DComp final visual；每个新 Flutter generation
  只消费一次锁定 V1 D3D11 keyed-mutex lease，并复制到 runner 私有 UI cache；
- 原生文件选择器；
- standalone native 模块中的 D3D11VA provider、独立 decode device 和稳定 shared snapshot frame storage；
- runner-owned D3D11 BGRA8 complete-viewport target ring 状态机；
- `native/windows/presentation/windows_presentation_backend.*` 中已激活的 D3D11 target lifecycle backend；
- `windows_d3d11_viewport_renderer.*` 中已验证的 D3D11VA/CPU YUV/BGRA、shared layout 与 SDR/scRGB viewport shader；
- H.264/H.265/MPEG-2 D3D11VA、multi-track split、play/pause、seek、step、loop、layout 和 native capture 的 runner 集成。

runner 直接编译并链接 shared native renderer、FFmpeg、D3D11 与 DComp。当前产品 target
是 SDR BGRA8；HDR/scRGB product policy、device-loss recovery 和更完整的 adapter diagnostics
仍是后续 stabilization 工作。构建强制要求 patched Flutter local engine；普通 Flutter SDK
不提供 surface-export ABI，也不能以 Flutter Texture 作为视频 fallback。

## 平台边界

```text
Dart UI / input
  -> MethodChannel / EventChannel
  -> WindowsNativePlayer -> shared Renderer
  -> WindowsD3D11PresentationBackend -> runner-owned video target ring

Flutter engine -> exported premultiplied-alpha UI surface
  -> keyed-mutex acquire once per generation -> runner-owned UI cache

video target + Flutter UI surface
  -> passive WindowsNativeCompositor / DComp
  -> HWND
```

### 播放帧与交互帧

Windows 与 macOS 一样把两种 cadence 分开：

```text
video clock / decoded PTS
  -> shared Renderer playback lane
  -> 新 source frame

pointer / wheel / layout intent
  -> WindowsViewportPresentationController (latest-only, max 2 in flight)
  -> 对 cached source frame 重新做 viewport projection

两条 lane -> runner-owned video target ring
          -> WindowsNativeCompositor
          -> DXGI Present(1) / display cadence
```

因此 24/30/60fps 视频不会限制 zoom、pan、split 等交互的上屏频率。runner 从当前
monitor mode 读取 nominal refresh 仅用于 diagnostics；真正的节拍由 compositor 的
DXGI `Present(1)` 提供。在 120Hz 显示器上，连续交互可约每 8.3ms 提交一次。D3D11
viewport backend 为每个 track 保留 presentation-device source cache：同一视频帧的
交互重投影不重复跨 D3D11VA device 复制，只更新 layout shader constants 并绘制新
target。Windows 与 macOS 都使用 6 个 native presentation targets；短暂 ring
backpressure 会按 latest layout intent 合并并重试，不计作交互失败。compositor 完成
GPU 消费后的 target 由 interaction callback 的安全直返路径或独立 native release
queue 回收，不能依赖正在处理 pointer/MethodChannel 的 Win32 UI 消息泵。

1. Windows presentation 策略只通过 `windows_presentation_backend.*` 的 D3D11 backend 边界进入。
2. runner 负责窗口、target ring、surface lease 和最终合成，不接管 Flutter frame 调度。
   与 macOS `MacOSNativeCompositorView` 同构：最终 compositor 不参与 hit-test，
   不拦截 pointer/keyboard/gesture，不调用 Flutter frame request/pump；它只采样
   Flutter 自己已经发布的最新 premultiplied-alpha surface。native video frame 只与
   runner-owned UI cache 合成，不重新 acquire 未更新的 keyed-mutex ring slot。
3. shared renderer 只提交 `RendererDrawSnapshot`，不暴露 GPU device、shared ring 或
   external-target draw 旁路。
4. color、layout、HDR、device-loss 和 UI smoke 使用 Windows 独立验证矩阵。

当前产品链路是 D3D11VA + D3D11。runner 分配 BGRA8 target ring，native backend
只绘制完整 viewport；runner 使用 Flutter engine
现有 V1 D3D11 keyed-mutex lease 更新 runner-owned UI cache，并按 Flutter 报告的
物理 viewport rect 定位 video surface。V2 的 D3D12 标签在 fork 提供真实
D3D12 resource/fence 之前不接入。

旧 capture-based DComp compositor、source projection、FP16 ring、window capture 和
renderer C FFI 已删除。需要算法参考时看历史提交，不要把旧文件复制回 active tree。

相关合同见 [Sandwich rendering](../native/docs/SANDWICH_RENDERING.md) 和
[Native architecture](../native/docs/ARCHITECTURE.md)。
