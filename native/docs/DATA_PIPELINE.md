# 数据管线

> 本文档描述当前 frame/data path。Windows 和 macOS 共用播放与调度管线，平台差异收敛在 hardware decode provider 与 `PresentationBackend`。

## 总览

```text
File
  -> DemuxThread
  -> PacketQueue
  -> DecodeThread
  -> FrameConverter
  -> TextureFrame
  -> TrackBuffer / BidiRingBuffer
  -> RenderSink
  -> PresentDecision
  -> RendererDrawSnapshot
  -> PresentationBackend
     -> Windows: wgpu/D3D12 import + DComp/DXGI present bridge
     -> macOS: wgpu/Metal import + CVPixelBuffer / IOSurface target
```

`DemuxThread` 保留 packet stream time base。`DecodeThread` 输出 frame 时转成微秒，之后 renderer、seek、loop、
layout 和 diagnostics 都使用微秒时间戳。

## TextureFrame

`TextureFrame` 是 decode/convert 之后进入 renderer buffer 的平台中立 frame 包装。当前主字段是
`FrameStorage` variant，兼容字段仍为旧调用点和测试保留。

| Storage | 典型来源 | 消费方 |
| --- | --- | --- |
| `CpuNv12FrameStorage` / planar YUV | software decode、hwdownload fallback | wgpu upload 或 legacy D3D11 upload |
| `CpuRgbaFrameStorage` | 旧测试、BGRA fallback、capture helpers | wgpu BGRA upload path 或 legacy D3D11 BGRA upload path |
| `D3D12TextureFrameStorage` / D3D12 texture | Windows D3D12VA renderer-owned path | wgpu/D3D12 backend |
| `D3D11Nv12FrameStorage` / D3D11 texture | legacy Windows D3D11VA renderer-owned path | legacy D3D11 backend |
| macOS CVPixelBuffer storage | VideoToolbox zero-copy path | wgpu-metal backend through CVMetalTextureCache / IOSurface |

Frame storage 必须带足 lifetime 信息。D3D11VA 和 VideoToolbox 硬解 frame 都持有底层 FFmpeg/CVPixelBuffer 引用，避免
decoder pool 在 renderer 使用期间提前复用 surface。

## PresentDecision 与 RendererDrawSnapshot

`RenderSink::evaluate()` 生成 `PresentDecision`，决定当前每轨该显示哪个 frame，以及 carry-forward、slot order、
layout、file id、track generation 等身份信息。`Renderer` 再把 decision 与 track/layout/color metadata 打包成
immutable `RendererDrawSnapshot`。

`PresentationBackend::draw_frame(snapshot)` 是平台边界：

- backend 可以上传、包装、复制或采样 snapshot 中已经选好的 frame；
- backend 不能选择播放时间，不能拥有 seek/loop/track lifecycle；
- draw 成功/失败、last error、storage kind、upload counters 会进入 diagnostics。

## Windows wgpu/D3D12 输出路径

```text
TextureFrame
  -> WgpuD3D12PresentationBackend
  -> D3D12VA texture import or CPU upload
  -> wgpu layout/color/source-projection/Flutter composition
  -> final target handed to the Windows present bridge
  -> DComp/DXGI window presentation
```

Windows 迁移目标是 D3D12VA decode + wgpu/D3D12 render core + runner-owned
DComp/DXGI present。D3D11 headless/shared BGRA texture path 只作为 legacy
compatibility 和测试 canary 保留；产品视频上屏不得回到 Flutter Texture SDR。

## macOS Metal / CVPixelBuffer 输出路径

```text
TextureFrame
  -> RendererDrawSnapshot
  -> wgpu/Metal PresentationBackend
     -> CVPixelBuffer fast path for VideoToolbox frames
     -> YUV/BGRA present package for software or fallback frames
  -> renderer-owned CVPixelBuffer / IOSurface
  -> Flutter Texture
```

VideoToolbox H.264/H.265 支持路径会保留 decoder-owned `CVPixelBuffer`，wgpu-metal backend 通过
`CVMetalTextureCache` / IOSurface 采样，避免 hwdownload。unsupported codec、unsupported format 或 software decode
走显式 present-package path，并在 diagnostics 中报告 `presentationFallbackReason` 与 storage kind。

## Capture 与 Diagnostics

Windows 和 macOS 都应通过 shared renderer/capture contract 观察当前 front buffer。macOS 仍保留 Flutter texture 侧
viewport metrics，但 native capture smoke 应优先使用 renderer/backend capture contract。

关键 diagnostics：

- backend kind、scheduler kind、storage kind、fallback reason
- upload count/failure count/last draw error
- per-track decode stats、offset、file id、slot
- presented PTS trace、large-gap count、host interval、renderer-owned ratio

## 内存量级

| 资源 | 1080p 量级 |
| --- | --- |
| PacketQueue 100 slots | 约 1 MB，取决于压缩码率 |
| RGBA/BGRA frame | 约 8 MB/帧 |
| NV12 frame | 约 3 MB/帧 |
| P010 frame | 约 6 MB/帧 |
| D3D11 headless BGRA 三缓冲 | 约 24 MB |
| macOS CVPixelBuffer target | 约 8 MB/1080p BGRA target |
| renderer-owned YUV/P010 textures or staging | 随格式、轨道数和目标尺寸变化 |
