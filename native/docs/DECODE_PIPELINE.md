# 解码管线

## 管线组成

```
File -> DemuxThread -> PacketQueue -> DecodeThread -> FrameConverter -> TrackBuffer
```

## DemuxThread

头文件: `media/demux_thread.h`

职责：

- 打开媒体输入并发现 video/audio stream
- 按 `DemuxStreamKind` 将 `AVPacket` 分发到已注册的 `PacketQueue`
- audio route 当前只选择第一个 `AVMEDIA_TYPE_AUDIO` stream，其他音轨 packet 会被丢弃
- 保持 packet 原始 stream time base；DecodeThread 在 frame 输出时转成微秒
- 轮询 `SeekController`，执行 `av_seek_frame` 后 flush packet queue
- EOF 后等待 seek，而不是让线程立即失去复用机会

## DecodeThread

头文件: `decode/decode_thread.h`

职责：

- 创建并打开 FFmpeg decoder
- 在 `start()` 前尝试 `enable_hardware_decode()`
- 消费 `PacketQueue`，输出 `TextureFrame`
- 维护 seek 后 preroll、exact seek discard/reorder、pause-after-preroll
- 写入 `TrackBuffer`，驱动 renderer 从 Buffering 进入 Ready

### Decoder 选择

- AV1 优先使用 FFmpeg 原生 `av1` decoder 进行 D3D11VA 协商；硬解不可用时，软件回退优先 `libdav1d`。
- VP9 不再跳过 D3D11VA；支持硬解的机器会先走 VP9 D3D11VA，失败再回退软件。
- 其他 codec 使用 `avcodec_find_decoder(codec_id)`，硬解失败时回退同 decoder 的软件路径。

### 硬解启用

```cpp
bool enable_hardware_decode(DecodeDeviceMode mode = DecodeDeviceMode::IndependentDevice,
                            void* render_device = nullptr,
                            std::recursive_mutex* device_mutex = nullptr);
```

`DecodeDeviceMode` 显式表达硬解设备策略：

| Mode | 用途 |
|------|------|
| `IndependentDevice` | H.264/H.265 等 renderer-owned NV12 路径，默认创建独立 D3D11 decode device |
| `FfmpegOwnedHwDownloadDevice` | AV1/VP9 hwdownload 路径，由 FFmpeg 自行创建 D3D11VA device/context |
| `SharedRenderDevice` | 诊断/实验用，必须显式传入 render device；不作为默认稳定路径 |

`avcodec_open2()` 延迟到 `start()` 中执行，确保 `hw_device_ctx`、`get_format` 和 `extra_hw_frames` 已经设置好。

硬解成功后存在两种输出路径：

| Codec/路径 | `FrameConverter` | 说明 |
|------------|------------------|------|
| H.264/H.265 等 renderer-owned surface | `download_to_cpu=false` | D3D11VA NV12 surface 进入 renderer，renderer 复制到自有 NV12 texture 后 shader 采样 |
| AV1/VP9 hwdownload | `download_to_cpu=true` | D3D11VA 负责解码，`av_hwframe_transfer_data` 下载到 CPU，再 sws 转 RGBA 上传 |

`extra_hw_frames=48` 只给 renderer-owned surface 路径配置。AV1/VP9 hwdownload 会尽快释放 decoder surface，强行扩大池子反而可能在部分驱动上产生黑帧。

## FrameConverter

头文件: `decode/frame_converter.h`

```cpp
bool init_software(int src_w, int src_h, AVPixelFormat src_fmt);
bool init_hardware(void* d3d_device, void* d3d_context,
                   int src_w, int src_h, HwDecodeType hw_type,
                   bool download_to_cpu);
```

### 软件路径

```
AVFrame(YUV/etc) -> sws_scale -> RGBA CPU buffer -> TextureFrame(cpu_data)
```

### 硬解 hwdownload 路径

```
AVFrame(D3D11VA) -> av_hwframe_transfer_data -> sws_scale -> RGBA CPU buffer
```

用于 AV1/VP9。它不是软件解码；只是上屏前把硬解结果转成稳定的 RGBA 上传路径。

### 硬解 renderer-owned 路径

```
AVFrame(D3D11VA NV12) -> TextureFrame(is_nv12, hw_frame_ref) -> renderer copy/sampling
```

`hw_frame_ref` 通过 `av_frame_ref` 持有 FFmpeg frame，避免 render thread 使用时 decoder pool 提前复用该 surface。

## D3D11VAProvider

头文件: `decode/hw/d3d11va_provider.h`

- `probe()` 根据 decoder `AVCodecHWConfig` 检查 D3D11VA 支持。
- AV1/VP9 hwdownload 路径使用 `FfmpegOwnedHwDownloadDevice`，让 FFmpeg 自己创建 D3D11VA device/context，以匹配 FFmpeg CLI 的稳定路径。
- H.264/H.265 等 renderer-owned 路径使用 `IndependentDevice`，创建独立 decode device 和带 `DECODER|SHADER_RESOURCE|MISC_SHARED` 的 surface。
- `SharedRenderDevice` 会记录 warn 日志，仅用于诊断共享 render device 的历史问题，不应作为常规播放路径。
- D3D11 immediate context 通过 mutex 串行化，避免解码和渲染线程并发访问导致驱动内部状态损坏。

## Seek 内的解码行为

`notify_seek()` 后 DecodeThread 会进入 Buffering：

1. 清理旧输出帧和 exact seek 临时状态。
2. 非 fresh codec seek 时 flush codec buffer。
3. Exact seek 丢弃目标前的帧，并保留目标前最后一帧作为暂停预览候选。
4. 硬解 exact seek 会轻微 pacing，避免 paused HEVC burst feeding 触发驱动不稳定。
5. post-seek preroll 达到阈值后设置 Ready。
