# Windows 宿主层

Windows 已恢复可交互的 native SDR 与 HDR/scRGB 播放路径。视频不经过 Flutter Texture；
runner 使用 DComp 合成 shared renderer 输出的 D3D11 视频 target 与 Flutter engine
导出的完整 premultiplied-alpha UI surface。

## 当前可用内容

- Win32 Flutter runner、窗口生命周期和插件注册；
- `video_renderer` MethodChannel/EventChannel 的 Windows native player facade；
- 顶层 HWND 上 input-transparent 的 DComp final visual；每个新 Flutter generation
  只消费一次锁定 V1 D3D11 keyed-mutex lease，并复制到 runner 私有 UI cache；
- 原生文件选择器；
- standalone native 模块中的 D3D11VA provider、独立 decode device 和稳定 shared snapshot frame storage；
- runner-owned D3D11 BGRA8 / RGBA16F complete-viewport target ring 状态机；
- `native/windows/presentation/windows_presentation_backend.*` 中已激活的 D3D11 target lifecycle backend；
- `windows_d3d11_viewport_renderer.*` 中已验证的 D3D11VA/CPU YUV/BGRA、shared layout 与 SDR/scRGB viewport shader；
- H.264/H.265/MPEG-2 D3D11VA、multi-track split、play/pause、seek、step、loop、layout 和 native capture 的 runner 集成；
- 基于当前 HWND output 的 DXGI HDR 状态、presentation adapter 和
  `DISPLAYCONFIG_SDR_WHITE_LEVEL` 的 Auto SDR/scRGB policy。
- runner message pump 到 `ActionRegistry` 的独立应用快捷键 channel；原始按键仍进入
  Flutter 文本输入，Windows `HardwareKeyboard` 对已接管快捷键只去重、不重复执行。

runner 直接编译并链接 shared native renderer、FFmpeg、D3D11 与 DComp。构建强制要求
patched Flutter local engine；普通 Flutter SDK
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
  -> passive WindowsNativeCompositor
     SDR: BGRA8 + RGB_FULL_G22_NONE_P709
     HDR: RGBA16F + RGB_FULL_G10_NONE_P709
  -> DComp
  -> HWND
```

runner diagnostics 同时导出 Flutter surface export 的 request、schedule、vsync、present
与 publish 计数，用来区分 engine frame pump、framework ticker 和 native compositor
消费。暂停状态下可通过 `resetNativePerfCounters` 重置 publish 采样窗口；inactive viewport
子树必须关闭动画 ticker，不能仅依赖 `IndexedStack` 隐藏后继续按显示刷新率发布 UI surface。

### HDR 与 SDR UI 合成

Windows HDR 使用 linear scRGB 作为 native video 与最终 swap chain 的工作域，不把
Flutter engine 的 surface 改成 FP16。两条输入合同始终分开：

```text
native video
  SDR output -> shared renderer tone-map -> BGRA8 target ring
  HDR output -> shared renderer linear scRGB -> RGBA16F target ring

Flutter UI
  engine full surface -> BGRA8 premultiplied sRGB keyed-mutex lease
                      -> runner-owned BGRA8 cache

final compositor in HDR mode
  Flutter: unpremultiply -> sRGB EOTF -> DISPLAYCONFIG SDR white / 80 -> premultiply
  video:   sample already-linear RGBA16F scRGB
  blend:   Flutter over video -> RGBA16F scRGB DComp swap chain
```

Flutter 传给 shared renderer 的 viewport 背景色也同步给 final compositor 和 Win32
client erase fallback。视频矩形之外不使用硬编码黑色；SDR 直接使用该 sRGB 颜色，scRGB
先按与 Flutter UI 相同的 SDR white 映射转为线性值，避免侧栏或窗口布局变化时暴露黑底。

这样 Windows HDR 开启时，Flutter 的 SDR UI 仍由系统 SDR white level 映射；不会因为
整张 Flutter surface 被误标成 linear FP16 而变暗。Flutter lease 不是完整 BGRA8
premultiplied-alpha surface 时 compositor 直接拒绝，不能退回 Flutter Texture、截图或
color-key 路径。

默认 `VOIDPLAYER_WINDOWS_PRESENTATION_MODE=auto`：只有 media 含 PQ/HLG track、当前
output 的 HDR 已开启且 output adapter 与 presentation device 匹配时才选择
`native-compositor-scrgb`；其余情况保持 `native-compositor-sdr`。开发诊断可显式设为
`native-compositor-sdr` 或 `native-compositor-scrgb`。scRGB swap chain、color space 或
RGBA16F target ring 建立失败时先显式回落 native SDR；native SDR 也失败则 player 创建
失败，绝不伪装成正常 Texture 播放。窗口跨显示器、HDR 设置、DPI 和 display topology
变化会重新探测并原地升降级 target ring。

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
first-frame gate 激活前，layout intent 只更新 shared renderer 状态，不提交尚无 cached
source frame 的交互重投影；首张 preview 会直接使用最新 layout，且不会把初始化等待误计为
presentation draw failure。D3D11
viewport backend 为每个 track 保留 presentation-device source cache：同一视频帧的
交互重投影不重复跨 D3D11VA device 复制，只更新 layout shader constants 并绘制新
target。Windows 与 macOS 都使用 6 个 native presentation targets；短暂 ring
backpressure 会按 latest layout intent 合并并重试，不计作交互失败。compositor 完成
GPU 消费后的 target 全部由同一条独立 native release queue 回收，不能依赖正在处理
pointer/MethodChannel 的 Win32 UI 消息泵，也不允许不同 callback 来源分叉回收规则。
若 shared renderer 在 draw 完成后判定 layout revision 已过期，则不发布旧帧，并在 shared
presentation completion 边界直接回收该 completed target，避免静默耗尽 ring。

presentation timing 保留端到端 `total` 供诊断，但性能健康判定只使用逐样本拆出的
`work`：frame callback/compositor `Present(1)` 与 backend 显式 GPU completion wait 分别
记为 callback/wait，不得再次算作 CPU/GPU 提交压力。这样同步到 60/120Hz 显示节拍不会
制造压力告警；真正过慢的 backend work、可见掉帧和 backpressure 仍由独立指标告警。

1. Windows presentation 策略只通过 `windows_presentation_backend.*` 的 D3D11 backend 边界进入。
2. runner 负责窗口、target ring、surface lease 和最终合成，不接管 Flutter frame 调度。
   与 macOS `MacOSNativeCompositorView` 同构：最终 compositor 不参与 hit-test，
   不拦截 pointer/keyboard/gesture，不调用 Flutter frame request/pump；它只采样
   Flutter 自己已经发布的最新 premultiplied-alpha surface。native video frame 只与
   runner-owned UI cache 合成，不重新 acquire 未更新的 keyed-mutex ring slot。
3. shared renderer 只提交 `RendererDrawSnapshot`，不暴露 GPU device、shared ring 或
   external-target draw 旁路。
4. color、layout、HDR、device-loss 和 UI smoke 使用 Windows 独立验证矩阵。

当前产品链路是 D3D11VA + D3D11。runner 按 presentation policy 分配 BGRA8 或
RGBA16F target ring，native backend 只绘制完整 viewport；runner 使用 Flutter engine
现有 V1 D3D11 keyed-mutex lease 更新 runner-owned UI cache，并按 Flutter 报告的
物理 viewport rect 定位 video surface。V2 的 D3D12 标签在 fork 提供真实
D3D12 resource/fence 之前不接入。

旧 capture-based DComp compositor、window capture 和 renderer C FFI 已删除。当前
RGBA16F ring 是 native D3D11 presentation backend 的 active HDR target，不是 Flutter
Texture 或旧 capture 路径。需要算法参考时看历史提交，不要把旧文件复制回 active tree。

相关合同见 [Sandwich rendering](../native/docs/SANDWICH_RENDERING.md) 和
[Native architecture](../native/docs/ARCHITECTURE.md)。
