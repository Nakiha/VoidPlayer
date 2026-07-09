# 解码管线

## 管线组成

```text
File
  -> DemuxThread
  -> PacketQueue
  -> DecodeThread
  -> FrameConverter
  -> TextureFrame
  -> TrackBuffer
```

解码管线本身是平台中立的；平台差异由 hardware decode provider 和 `FrameConverter` 输出的 storage kind 表达。

## DemuxThread

头文件：`media/demux_thread.h`

职责：

- 打开媒体输入并发现 video/audio stream；
- 按 `DemuxStreamKind` 将 `AVPacket` 分发到已注册的 `PacketQueue`；
- audio route 当前只选择第一个 audio stream；
- 保持 packet 原始 stream time base，frame 输出时转成微秒；
- 轮询 `SeekController`，执行 `av_seek_frame` 后 flush packet queue；
- EOF 后等待 seek，而不是让线程立即失去复用机会。

## DecodeThread

头文件：`renderer/decode/decode_thread.h`

职责：

- 创建并打开 FFmpeg decoder；
- 在 `start()` 前通过 hardware provider 尝试硬解；
- 消费 `PacketQueue`，输出 `TextureFrame`；
- 维护 preroll、exact seek discard/reorder、pause-after-preroll；
- 写入 `TrackBuffer`，驱动 renderer 从 Buffering 进入 Ready。

## Hardware Providers

| 平台 | Provider | 常规用途 |
| --- | --- | --- |
| Windows | D3D11VA | H.264/H.265 renderer-owned surface；AV1/VP9 可走 hwdownload fallback |
| macOS | VideoToolbox | H.264/H.265 CVPixelBuffer preservation；unsupported codec/format 显式 fallback |

Provider 只决定 decoder device 与 hardware frame output。它不决定播放时间、seek、loop、layout 或 track lifecycle。

## DecodeDeviceMode

`DecodeDeviceMode` 表达硬解设备策略：

| Mode | 用途 |
| --- | --- |
| `IndependentDevice` | 默认稳定路径；Windows 创建独立 D3D11 decode device，macOS 创建 VideoToolbox context |
| `FfmpegOwnedHwDownloadDevice` | Windows AV1/VP9 等 hwdownload 路径，让 FFmpeg 自行管理 hardware context |
| `SharedRenderDevice` | 诊断/实验路径，不作为常规播放默认值 |

`avcodec_open2()` 延迟到 `start()`，确保 `hw_device_ctx`、`get_format` 和 frame pool 配置已完成。

## FrameConverter 输出路径

头文件：`renderer/decode/frame_converter.h`

`FrameConverter` 将 `AVFrame` 转为 `TextureFrame`。不支持的格式、非法几何尺寸、hwdownload 失败或 CPU pack 失败会返回
`std::nullopt`，`DecodeThread` 随后进入错误状态，避免空 frame 进入 renderer。

| 路径 | 数据流 | 说明 |
| --- | --- | --- |
| Software fallback | `AVFrame(YUV/NV12/P010) -> CPU planar/NV12/P010 TextureFrame` | 显式 fallback；不依赖 libswscale/libyuv 做通用转换 |
| Windows D3D11VA renderer-owned | `AVFrame(D3D11VA) -> D3D11 TextureFrame` | D3D11 backend 复制/采样 renderer-owned surface |
| Windows hwdownload | `AVFrame(D3D11VA) -> av_hwframe_transfer_data -> CPU NV12/P010` | 用于需要稳定 CPU upload fallback 的 codec/driver 路径 |
| macOS VideoToolbox zero-copy | `AVFrame(VideoToolbox) -> CVPixelBuffer TextureFrame` | native-metal backend 通过 IOSurface/CVMetalTextureCache 上屏 |
| macOS fallback package | `AVFrame -> CPU YUV/BGRA package` | VVC/software 或 unsupported format 的 renderer-owned native-metal package path |

当前 runtime 不引入 `libswscale` / `libyuv` 作为 broad fallback。新增像素格式必须做确定性 pack/shader 支持，并补颜色一致性测试。

## Decoder 选择

- AV1 优先使用 FFmpeg 原生 `av1` decoder 进行硬解协商；硬解不可用时软件回退优先 `libdav1d`。
- VP9 不跳过硬解探测；支持硬解的机器先试 hardware provider，失败再回退软件。
- H.264/H.265 在 Windows 走 D3D11VA，在 macOS 走 VideoToolbox；支持时保留 platform hardware frame 进入 renderer-owned backend。
- VVC/H.266 当前在 macOS VideoToolbox path 明确 decline，走 software decode + renderer-owned package fallback，并在 diagnostics 中报告 `software-decode`。

## Zero-Copy 与 Fallback 边界

Zero-copy 是 presentation backend 能直接消费 decoder-owned frame 时的优化，不改变 decode/track/playback policy。

- Windows：D3D11VA surface 由 D3D11 backend 消费；必要时复制到 renderer-owned texture，避免 decoder pool lifetime 问题。
- macOS：VideoToolbox `CVPixelBuffer` / IOSurface 由 native-metal backend 消费；fallback reason 必须可见。
- hwdownload fallback：仍可使用 hardware decoder，但 presentation 前下载到 CPU。
- software fallback：decoder 本身是 software，presentation 仍走 renderer-owned backend。

## Seek 内的解码行为

`notify_seek()` 后 DecodeThread 进入 Buffering：

1. 清理旧输出帧和 exact seek 临时状态；
2. 非 fresh codec seek 时 flush codec buffer；
3. Exact seek 丢弃目标前的帧，并保留目标前最后一帧作为暂停预览候选；
4. 硬解 exact seek 会轻微 pacing，避免 paused HEVC burst feeding 触发驱动不稳定；
5. post-seek preroll 达到阈值后设置 Ready。

Exact seek 的容器/码流限制见 [SEEK_STRATEGY.md](SEEK_STRATEGY.md)。色彩和 HDR/SDR 边界见
[COLOR_PIPELINE.md](COLOR_PIPELINE.md)。
