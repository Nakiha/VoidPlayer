# Native 模块架构概览

> 本文档描述 back-to-native sandwich 分支的目标架构。

## 模块定位

VoidPlayer native 保留共享媒体播放与渲染调度内核，重启平台 presentation：

- shared demux/decode/playback/seek/loop/track/layout/render scheduler
- shared audio engine / miniaudio output
- shared diagnostics / capture / UI automation hooks
- macOS native Metal video target
- Windows D3D11VA decode/shared-snapshot、runner-owned target ring 与 D3D11 target lifecycle backend；viewport shader/runner composition 仍 fail-closed
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
├── media/               # demux, packet queue, seek controller
├── audio/               # shared audio engine / miniaudio output
├── playback/            # playback controller and clock/audio coordination
├── renderer/            # shared scheduler, decode, buffer, render contracts
│   ├── decode/          # DecodeThread, FrameConverter, hardware providers
│   ├── render/          # RendererDrawSnapshot, PresentationBackend contracts
│   ├── sync/            # RenderSink and present scheduling
├── windows/             # fail-closed Windows presentation factory boundary
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
| `FrameConverter` | AVFrame 到 native-target frame storage；硬解 import 只在已实现 backend 上启用 |

## 数据流总览

```text
Media file
  -> DemuxThread
  -> PacketQueue
  -> DecodeThread
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

| 平台 | 硬解 provider | presentation backend |
| --- | --- | --- |
| Windows | D3D11VA；H.264/H.265 使用独立 decode device 与稳定 shared snapshot，AV1/VP9 可 hwdownload | `native-d3d11` target lifecycle active；viewport shader/runner composition 尚未接通 |
| macOS | VideoToolbox CVPixelBuffer or explicit fallback package | native Metal target backed by CVPixelBuffer / IOSurface |

## 当前播放路径状态

- macOS native Metal presentation builds and passes native smoke.
- macOS VideoToolbox preserves native-target CVPixelBuffer frames when supported.
- Windows native decode foundation is active in standalone native builds；presentation
  remains fail-closed until the runner-composed D3D sandwich is implemented.
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
