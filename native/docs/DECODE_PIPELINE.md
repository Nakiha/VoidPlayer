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
| AV1/VP9 hwdownload | `download_to_cpu=true` | D3D11VA 负责解码，`av_hwframe_transfer_data` 下载到 CPU，再打包/上传 NV12 |

`extra_hw_frames=48` 只给 renderer-owned surface 路径配置。AV1/VP9 hwdownload 会尽快释放 decoder surface，强行扩大池子反而可能在部分驱动上产生黑帧。

## FrameConverter

头文件: `decode/frame_converter.h`

```cpp
bool init_software(int src_w, int src_h, AVPixelFormat src_fmt);
bool init_hardware(void* d3d_device, void* d3d_context,
                   int src_w, int src_h, HwDecodeType hw_type,
                   bool download_to_cpu);
std::optional<TextureFrame> convert(AVFrame* frame);
```

`convert()` 失败时返回 `std::nullopt`，不会再返回空 `TextureFrame`。不支持的像素格式、非法几何尺寸、hwdownload 失败或 CPU NV12 打包失败都会进入这个路径；DecodeThread 收到失败后将 `TrackBuffer` 设置为 `TrackState::Error`，避免空 `texture_handle` 被推入 buffer 后表现成黑帧。

当前播放器 runtime 不依赖也不引入 `libswscale` / `libyuv` 作为通用 fallback。FrameConverter 支持 `YUV420P`、`YUVJ420P`、`NV12`、`NV21`、`YUV422P`、`YUVJ422P`、`YUV444P`、`YUVJ444P`、`YUV420P10LE`、`P010LE`、`YUV422P10LE`、`YUV444P10LE` 到 CPU NV12/P010 路径。不支持的格式按显式错误处理。

这个限制是有意的：FrameConverter 只做确定性的 pack/upload，不做通用色彩转换。新增像素格式时应逐个实现转换或 shader 路径，并补软解/硬解颜色一致性回归，避免同一个片源在两条解码路径上出现颜色差异。

色彩和 HDR/SDR 边界见 [COLOR_PIPELINE.md](COLOR_PIPELINE.md)。

## Shader 常量布局

多轨渲染 shader 仍通过运行时 `D3DCompile` 编译内嵌 HLSL。Windows runner/native 发布包需要确保系统可加载 D3DCompiler 运行时；若后续遇到分发问题，再评估预编译 shader blob。

`multitrack.hlsl` 的 `cbuffer Constants` 对应 C++ `video_renderer/shader_constants.h` 中的 `ShaderConstants`。该头文件包含 304-byte size 和关键 offset `static_assert`，native 单测也会校验布局，避免 C++ 字段移动后 shader 读错 uniform。

### 软件路径

```
AVFrame(YUV/NV12) -> CPU NV12 buffer -> TextureFrame(cpu_data, is_nv12)
```

### 硬解 hwdownload 路径

```
AVFrame(D3D11VA) -> av_hwframe_transfer_data -> CPU NV12 buffer
```

用于 AV1/VP9。它不是软件解码；只是上屏前把硬解结果转成稳定的 CPU NV12 上传路径。

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

Exact seek 的容器/码流限制见 [SEEK_STRATEGY.md](SEEK_STRATEGY.md)。特别是 H.264/FLV 如果只在 AVC sequence header 中保存 SPS/PPS，而 IDR 不重复参数集，flush 后从 seek 点开始解码可能缺少上下文。当前策略是记录 warning 并保持 best-effort，不自动注入参数集或静默降级。
