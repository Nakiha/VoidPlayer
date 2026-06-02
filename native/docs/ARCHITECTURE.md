# Native 模块架构概览

> 本文档是 native 模块入口。它描述当前架构，不记录迁移流水；详细实现历史以 git history 为准。

## 模块定位

VoidPlayer native 是一套共享媒体播放与渲染调度内核，加平台 presentation backend：

- shared demux/decode/playback/seek/loop/track/layout/render scheduler
- Windows presentation backend：D3D11 / shared texture / Flutter Texture
- macOS presentation backend：Metal / CVPixelBuffer / IOSurface / Flutter Texture
- shared audio engine：miniaudio 输出，Windows 与 macOS 使用各自系统设备后端
- shared diagnostics / capture / UI automation hooks

平台 runner 只负责 OS glue。Windows runner 负责 Win32/D3D11 texture bridge，macOS runner 负责
Cocoa、sandbox file access、platform channel、FlutterTexture、CVPixelBuffer lifecycle 和 frame
notification。播放策略、seek、loop、track lifecycle、layout、refresh completion 和 failure state 都属于
shared native code。

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
              -> platform PresentationBackend
                 -> D3D11 shared texture on Windows
                 -> Metal / CVPixelBuffer / IOSurface on macOS
```

`Renderer` 拥有 playback/render cadence、track selection、carry-forward、layout constants 和 present
decision。`PresentationBackend` 只消费 `RendererDrawSnapshot` / `PresentDecision`，把已经选好的帧变成平台纹理；
backend 不决定播放时间，也不拥有 track state。

## 目录结构

```text
native/
├── common/              # platform-neutral logging and shared helpers
├── media/               # demux、packet queue、seek controller
├── audio/               # shared audio engine / miniaudio device output
├── playback/            # playback controller、clock/audio coordination
├── video_renderer/      # shared renderer scheduler、decode、buffer、render contracts
│   ├── decode/          # DecodeThread、FrameConverter、hardware providers
│   ├── render/          # PresentDecision、RendererDrawSnapshot、PresentationBackend
│   ├── sync/            # RenderSink and present scheduling
│   └── exports/         # C FFI / Python binding surfaces
├── windows/             # Windows native facade, crash hooks, and D3D11 backend
│   ├── player/          # Windows NativePlayer facade
│   ├── d3d11/           # Windows D3D11 backend implementation
│   └── common/          # Windows process-global helpers
├── macos/               # macOS native bridge and Metal presentation backend
├── tests/               # Catch2 tests on Windows-oriented native targets
├── tools/               # native smoke binaries and CLIs
└── docs/
```

## 核心对象

| 对象 | 当前职责 |
| --- | --- |
| `NativePlayer` | Shared playback facade，平级协调 playback、renderer、audio、capture |
| `Renderer` | 共享 render scheduler，拥有 track lifecycle、seek/loop/layout command surface、present cadence |
| `RenderSink` / `PresentDecision` | 平台无关的多轨 frame selection、identity、carry-forward、layout decision |
| `RendererDrawSnapshot` | renderer 到 backend 的 immutable draw input |
| `PresentationBackend` | 平台 presentation seam；Windows 实现 D3D11，macOS 实现 Metal/CVPixelBuffer |
| `TrackPipeline` | 每轨 demux/decode/buffer state，使用 file id + generation 防止 remove/re-add 串帧 |
| `FrameConverter` | AVFrame 到 `TextureFrame`；保留硬解 surface 或做确定性 CPU pack |

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
  -> Flutter Texture / native capture
```

Windows 和 macOS 共用从 demux 到 `RendererDrawSnapshot` 的主路径。差异从 hardware decode provider 和
presentation backend 开始：

| 平台 | 硬解 provider | presentation backend |
| --- | --- | --- |
| Windows | D3D11VA | D3D11 shared texture / headless output / optional swap chain |
| macOS | VideoToolbox | Metal target backed by CVPixelBuffer / IOSurface |

## 当前播放路径状态

- Windows D3D11 path 是原始产品路径，仍需在 Windows host 上做 preservation gate。
- macOS native playback 已进入 stabilization / release-readiness：shared scheduling、renderer-owned Metal
  presentation、VideoToolbox zero-copy、software fallback、refresh completion、per-track diagnostics 都在 normal route。
- macOS software decode fallback 是显式诊断路径，不是隐藏主路径。
- 4K60 门槛仍属于 stabilization gate；先依赖 cadence diagnostics、fallback reason、upload stats 和 UI smoke 证据化。

## 文档索引

### Current Architecture

| 文档 | 内容 |
| --- | --- |
| [数据管线](DATA_PIPELINE.md) | 平台中立 frame/data path，Windows D3D11 与 macOS Metal 输出路径 |
| [解码管线](DECODE_PIPELINE.md) | D3D11VA、VideoToolbox、software fallback、hwdownload/zero-copy 边界 |
| [色彩管线](COLOR_PIPELINE.md) | YUV/RGB/P010、range/matrix、shader contract 和 parity gates |
| [线程模型](THREADING_MODEL.md) | 线程角色、锁顺序、render loop 与 callback 边界 |
| [时钟与同步](CLOCK_AND_SYNC.md) | Clock、倍速、A/V sync、loop timing |
| [缓冲设计](BUFFER_DESIGN.md) | PacketQueue、TrackBuffer、preroll、BidiRingBuffer |
| [Seek 策略](SEEK_STRATEGY.md) | seek controller、exact seek、preview publication |

### Platform Backend

| 文档 | 内容 |
| --- | --- |
| [Renderer 平台后端统一计划](RENDERER_PLATFORM_BACKEND_PLAN.md) | shared renderer + platform backend status/gates |
| [D3D11 后端](D3D11_BACKEND.md) | Windows D3D11 device、shared texture、capture、device-loss behavior |
| [macOS 移植计划](MACOS_PORT_PLAN.md) | macOS readiness、runner 边界、remaining gates |
| [macOS Presentation Backend](MACOS_PRESENTATION_BACKEND.md) | macOS renderer-owned Metal route, fallback adapter, refresh, and diagnostics contract |

### Readiness / Release / Tooling

| 文档 | 内容 |
| --- | --- |
| [构建与测试](BUILD_AND_TEST.md) | dev.py、CMake targets、macOS stabilization gates、Windows preservation、package checks |
| [FFI 与绑定](FFI_AND_BINDINGS.md) | C FFI、Python bindings、runtime ABI |
| [Target 边界](TARGET_BOUNDARIES.md) | CMake target boundaries and feature options |
| [Native 第三方清单](../THIRD_PARTY_NATIVE.md) | FFmpeg/zstd/spdlog/Catch2 license and package notes |

### Historical / Todo

| 文档 | 内容 |
| --- | --- |
| [Native Refactor Todo](NATIVE_REFACTOR_TODO.md) | 历史技术债与后续 refactor notes |
| [Native Stabilization Round](NATIVE_STABILIZATION_ROUND.md) | 历史 stabilization notes |
| [Native Stabilization History](NATIVE_STABILIZATION_HISTORY.md) | 历史 patch log |
