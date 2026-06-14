# Native 模块架构概览

> 本文档是 native 模块入口，只描述当前架构和合同。

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
├── renderer/            # shared renderer scheduler、decode、buffer、render contracts
│   ├── decode/          # DecodeThread、FrameConverter、hardware providers
│   ├── render/          # PresentDecision、RendererDrawSnapshot、PresentationBackend
│   ├── sync/            # RenderSink and present scheduling
│   └── exports/         # C FFI and pybind11 C++ binding surfaces
├── python/              # Python convenience package source for dist/python
├── windows/             # Windows native facade, crash hooks, and D3D11 backend
│   ├── player/          # Windows NativePlayer facade
│   ├── decode/          # Windows D3D11VA decode integration
│   ├── d3d11/           # Windows D3D11 backend, overlay, capture, and renderer compatibility hooks
│   └── common/          # Windows process-global helpers
├── macos/               # macOS native bridge and Metal presentation backend
├── examples/            # development-only demos and sample entrypoints
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

## Renderer ownership map

`Renderer` 是 public facade，`Renderer::Impl` 是 private composition root。新增 renderer 行为时优先落到
下面的 ownership 组件，不要把长逻辑继续塞进 `Renderer::Impl`：

| 行为/状态 | 首选 owner |
| --- | --- |
| Playback/time/loop range/seek gate | `RendererTimelineController` |
| Track lifetime、add/remove/recreate/seek/offset mutation | `RendererTrackMutationController` |
| Track storage、slot/file id、generation、cached duration | `RendererTrackRegistry` |
| Track snapshots、present decisions、paused preview、perf/memory diagnostics | `RendererTrackPresentationModel` |
| Draw snapshot、paused redraw、layout redraw、present completion | `RendererPresentCommandProcessor` |
| Render-thread cadence、preroll、paused preview scheduling、deadline sleep | `RendererRenderLoopCommandProcessor` + `RendererLoopDriver` |
| Backend/device/texture/capture/callback storage | `RendererPresentationController` |
| Layout revisions、pending layout intent、viewport compositor grace | `RendererLayoutState` |
| Native event publication | `RendererEventBus` |
| Presentation counters、timing、backpressure/device-loss diagnostics | `PresentationMetricsStore` |

`RendererTrackController` 目前仍是 compatibility facade，向 registry / mutation / presentation model 转发；
它不持有 renderer locks、不调用 host/platform callbacks。present/render-loop command context 中的
`*_locked` hooks 必须由调用方持有 `state_mutex_` 调用，详见 [线程模型](THREADING_MODEL.md)。

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
| [缓冲合同](BUFFERING.md) | PacketQueue、TrackBuffer、preroll、BidiRingBuffer |
| [Seek 策略](SEEK_STRATEGY.md) | seek controller、exact seek、preview publication |
| [Native Event Pipeline](NATIVE_EVENT_PIPELINE.md) | native -> Dart EventChannel 事实事件合同 |

### Platform Backend

| 文档 | 内容 |
| --- | --- |
| [D3D11 后端](D3D11_BACKEND.md) | Windows D3D11 device、shared texture、capture、device-loss behavior |
| [macOS Readiness](MACOS_READINESS.md) | macOS readiness、runner 边界、remaining gates |
| [macOS Presentation Backend](MACOS_PRESENTATION_BACKEND.md) | macOS renderer-owned Metal route, fallback adapter, refresh, and diagnostics contract |
| [macOS HDR Exploration](MACOS_HDR_EXPLORATION.md) | macOS native compositor HDR/EDR path, Flutter fork pin, and validation evidence |

### Readiness / Release / Tooling

| 文档 | 内容 |
| --- | --- |
| [构建与测试](BUILD_AND_TEST.md) | dev.py、CMake targets、macOS stabilization gates、Windows preservation、package checks |
| [FFI 与绑定](FFI_AND_BINDINGS.md) | C FFI、Python bindings、runtime ABI |
| [Target 边界](TARGET_BOUNDARIES.md) | CMake target boundaries and feature options |
| [Native 第三方清单](../THIRD_PARTY_NATIVE.md) | FFmpeg/zstd/spdlog/Catch2 license and package notes |

### Analysis

| 文档 | 内容 |
| --- | --- |
| [Analysis 模块](ANALYSIS_MODULE.md) | VAC2/VACHUNK generation、parsers、FFI、benchmarks |
| [Analysis Cache](ANALYSIS_CACHE.md) | VAC2 base + VACHUNK derived chunk cache contract |
| [Analysis Overlay](ANALYSIS_OVERLAY.md) | 主窗口 codec block overlay、native renderer、hit-test contract |
| [VAC2](formats/VAC2.md) | base analysis container format |
| [VACHUNK](formats/VACHUNK.md) | derived analysis chunk format |

### Documentation Rule

`native/docs/` only describes current behavior and contracts.
