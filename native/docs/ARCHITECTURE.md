# Native 模块架构概览

> 本文档描述 back-to-native sandwich 分支的目标架构。

## 模块定位

VoidPlayer native 保留共享媒体播放与渲染调度内核，重启平台 presentation：

- shared demux/decode/playback/seek/loop/track/layout/render scheduler
- shared audio engine / miniaudio output
- shared diagnostics / capture / UI automation hooks
- macOS native Metal video target
- Windows D3D11VA decode/shared-snapshot、runner-owned target ring、D3D11 viewport backend 与 passive DComp final composition
- runner-owned final composition of native video + Flutter ARGB UI

平台 runner 不再把视频伪装成 Flutter Texture 主路径，也不让 native backend
代 Flutter 控制上屏。Flutter 继续拥有自己的透明 UI surface；native renderer
只拥有视频纹理；runner 负责最终 compose。

## 当前架构

```text
Dart UI / Actions
  -> NativePlayerController
     -> platform channel glue
        -> shared NativePlayer facade
           -> shared PlaybackController / Clock / AudioEngine
           -> shared Renderer scheduler
              -> RenderSink / PresentDecision
              -> RendererDrawSnapshot
              -> native video PresentationBackend
                 -> runner-composed video layer
                    + Flutter engine ARGB UI layer
                    -> screen
```

See [SANDWICH_RENDERING.md](SANDWICH_RENDERING.md) for the presentation
contract.

## 目录结构

```text
native/
├── common/              # platform-neutral logging and helpers
├── media/               # demux, packet queue, seek controller, shared video decode session
├── audio/               # shared audio engine / miniaudio output
├── playback/            # playback controller and clock/audio coordination
├── renderer/            # shared scheduler, decode, buffer, render contracts
│   ├── decode/          # DecodeThread, FrameConverter, hardware providers
│   ├── render/          # RendererDrawSnapshot, PresentationBackend contracts
│   ├── sync/            # RenderSink and present scheduling
├── windows/             # D3D11VA、WindowsNativePlayer、D3D11 viewport backend
├── macos/               # macOS native bridge and Metal presentation backend
├── tests/               # Catch2 tests
├── tools/               # native smoke binaries and CLIs
└── docs/
```

## 核心对象

| 对象 | 当前职责 |
| --- | --- |
| `NativePlayer` | Shared playback facade，协调 playback、renderer、audio、capture |
| `Renderer` | 共享 render scheduler，拥有 track lifecycle、seek/loop/layout command surface、present cadence |
| `RenderSink` / `PresentDecision` | 平台无关的多轨 frame selection、identity、carry-forward、layout decision |
| `RendererDrawSnapshot` | renderer 到 native video backend 的 immutable draw input |
| `PresentationBackend` | native video texture/layer writer；不控制 Flutter 上屏 |
| `TrackPipeline` | 每轨 demux/decode/buffer state |
| `MediaInputSession` | 播放与离线 CLI 共用的同步输入生命周期：open/probe、stream metadata、read、seek/flush、interrupt 与 private CDN FLV；不拥有线程、packet queue 或播放策略 |
| `VideoDecodeSession` | 播放与离线 CLI 共用的 codec/hardware-device 生命周期、open fallback 和 send/receive；不拥有 seek、buffer 或 presentation state |
| `FrameConverter` | AVFrame 到 native-target frame storage；硬解 import 只在已实现 backend 上启用 |

### Exact playback pacing

Shared native playback uses a single `PlaybackPacingController` boundary.
`RendererTrackPresentationModel` supplies immutable per-track queue-capacity
and PTS-frontier facts; the pacing controller owns preroll, mid-stream
rebuffer hysteresis, and the effective clock rate. Platform runners do not
participate in playback admission.

- The user-requested speed and effective clock speed are separate.
- With no audible track, forward-buffer depletion may reduce effective speed
  before an underrun. The fill ratio is normalized against
  each track's attainable forward-frame target, so a full shallow hardware
  decode queue remains at the requested speed throughout the frame interval.
- PTS headroom is synchronization/frontier evidence, not a continuous clock
  control input. Frame phase therefore cannot make wall-clock playback run
  slow while every decoder queue is healthy.
- With audible audio, pacing remains at the requested speed and uses an
  audio/video hold instead of changing video rate without time stretching.
- The slowest currently active, non-EOF track is the pacing bottleneck.
- Positive-offset tracks do not constrain the clock before their global start.
- `RenderSink::evaluate()` is non-mutating. A native presentation submission
  must be accepted before `commit_presented()` advances queue cursors.
- Queue heads are committed in global PTS order. The scheduler never greedily
  skips decoded frames to catch a wall clock.

Viewport interaction remains display-linked and independent: while playback is
held for decode recovery, Windows/macOS runners continue reprojecting the last
complete native source frame.

## 数据流总览

```text
Media file
  -> MediaInputSession
  -> DemuxThread
  -> PacketQueue
  -> DecodeThread
  -> VideoDecodeSession
  -> FrameConverter
  -> TextureFrame
  -> TrackBuffer / BidiRingBuffer
  -> RenderSink::evaluate()
  -> PresentDecision
  -> RendererDrawSnapshot
  -> PresentationBackend::draw_frame()
  -> native video target
  -> runner composition with Flutter ARGB UI
```

Windows 和 macOS 共用从 demux 到 `RendererDrawSnapshot` 的主路径。差异从
hardware decode provider 和 presentation backend 开始：

`DemuxThread` 是实时输入 adapter：线程、packet queue routing、EOF 等待和
`SeekController` 属于播放策略，底层 open/probe/read/seek/interrupt 由
`MediaInputSession` 持有。`DecodeThread` 同样保留 packet queue、seek/preroll、
backpressure 和 `TrackBuffer` 状态，但 codec context 与硬解 provider 由
`VideoDecodeSession` 持有。离线 quality CLI 同步消费同一个 `MediaInputSession`，
使用自己的抽样策略，再调用同一个 `VideoDecodeSession`。因此 CLI 不依赖
renderer/Flutter，播放与分析也不会分别维护 FFmpeg 输入生命周期、codec open、
AV1 software fallback 或 guarded send/receive 实现。

| 平台 | 硬解 provider | presentation backend |
| --- | --- | --- |
| Windows | D3D11VA；H.264/H.265/AV1/VP9 使用独立 decode device 与稳定 single-slice shared snapshot | `native-d3d11` 在 presentation device 直接采样 opened snapshot，完整 viewport shader 写入 runner-owned BGRA8/FP16 ring；DComp 合成 Flutter UI |
| macOS | VideoToolbox CVPixelBuffer or explicit fallback package | native Metal target backed by CVPixelBuffer / IOSurface |

### 交互 presentation cadence

视频 source cadence 继续由 shared playback clock、PTS selection 与 render scheduler
决定；viewport interaction cadence 则由平台 runner 拥有。runner 把最新 layout
intent 应用到 shared `LayoutState`，并请求 backend 用最近的 source frame 重画，而不
等待下一张解码帧。macOS 由 display link 驱动，Windows 由 runner interaction
controller 提交并在 DXGI `Present(1)` 上按显示器节拍完成；两端最多允许两个交互帧
in flight。这样 shared 层仍只拥有 frame selection/layout snapshot 语义，显示器时钟、
GPU target ring 和最终 Flutter/native 合成都留在平台层。

Shared interaction refresh 返回 `Presented / NotReady / Failed` 三态。新增轨道尚未
preroll、完整多轨 `PresentDecision` 尚不可用时返回 `NotReady`；平台 display-linked
controller 只重试最新 layout revision，不把 readiness gap 记成 backend failure，也不
使用只覆盖部分活动轨道的旧 snapshot 上屏。

## 当前播放路径状态

- macOS native Metal presentation builds and passes native smoke.
- macOS VideoToolbox preserves native-target CVPixelBuffer frames when supported.
- Windows native decode、cross-device GPU snapshot bridge、SDR/scRGB viewport shader
  与 runner-composed D3D sandwich 已接通；当前产品 target 为 SDR BGRA8，HDR/scRGB policy
  与 device-loss recovery 仍处于 stabilization。
- Flutter premultiplied-alpha export remains a Flutter fork requirement, but
  Flutter should not own video presentation.

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [Sandwich rendering](SANDWICH_RENDERING.md) | 新 presentation 合同 |
| [macOS Presentation Backend](MACOS_PRESENTATION_BACKEND.md) | macOS native Metal backend |
| [数据管线](DATA_PIPELINE.md) | 平台中立 frame/data path |
| [解码管线](DECODE_PIPELINE.md) | hardware/software decode boundary |
| [色彩管线](COLOR_PIPELINE.md) | YUV/RGB/P010 and color metadata |
| [线程模型](THREADING_MODEL.md) | 线程角色、锁顺序、render loop 与 callback 边界 |
