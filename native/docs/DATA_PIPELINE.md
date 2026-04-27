# 数据管线

> 本文档描述帧数据从文件到屏幕的当前路径。

## 总览

```
File
  -> DemuxThread
  -> PacketQueue
  -> DecodeThread
  -> FrameConverter
  -> TrackBuffer / BidiRingBuffer
  -> RenderSink
  -> D3D11FramePresenter
  -> Renderer D3D11 draw
  -> SwapChain 或 D3D11HeadlessOutput shared texture
```

全链路时间戳使用微秒。DemuxThread 将 stream time base 转为 `{1, 1000000}` 后写入 packet/frame。

## TextureFrame

```cpp
struct TextureFrame {
    int64_t pts_us;
    int64_t duration_us;
    int width, height;
    bool is_ref;
    void* texture_handle;
    FrameStorage storage;
    bool is_nv12;
    int texture_array_index;
    shared_ptr<void> hw_frame_ref;
    shared_ptr<vector<uint8_t>> cpu_data;
};
```

字段含义：

- `storage` 是当前主路径，使用 `FrameStorage` variant 区分 `CpuRgba`、`D3D11Nv12`、`D3D11Texture`。
- `cpu_data`、`texture_handle`、`is_nv12`、`texture_array_index`、`hw_frame_ref` 仍保留为兼容字段，便于迁移期间的测试和旧调用点。
- `CpuRgbaFrameStorage` 持有软件路径或 hwdownload 路径产生的 RGBA 数据。
- `D3D11Nv12FrameStorage` 指向 D3D11VA NV12 texture 和 array slice，并持有 frame ref，保证 decoder surface 在 renderer 使用期间不被 FFmpeg pool 回收。

## 三条输出路径

| 路径 | 典型 codec | 数据流 | 特点 |
|------|------------|--------|------|
| 软件解码 | fallback、部分不支持硬解的 codec | `AVFrame -> sws_scale -> RGBA CPU -> D3D11 upload` | 最稳，CPU 成本高 |
| 硬解 hwdownload | AV1、VP9 | `D3D11VA decode -> av_hwframe_transfer_data -> RGBA CPU -> D3D11 upload` | 仍是硬解，避免直接采样驱动差异导致黑/灰帧 |
| 硬解 renderer-owned NV12 | H.264、H.265 等 | `D3D11VA NV12 -> renderer-owned NV12 texture -> shader NV12->RGB` | CPU 拷贝少，性能路径 |

## Renderer 上屏

Renderer 通过 `RenderSink::evaluate()` 选择每轨应该显示的帧。`D3D11FramePresenter` 根据 `TextureFrame::storage` 类型执行：

- RGBA CPU 数据：上传/复用每轨 RGBA texture 后按 RGBA 采样。
- NV12 硬解数据：复制 decoder surface 的目标 array slice 到 renderer-owned NV12 texture，再创建 Y/UV SRV 采样。

复制到 renderer-owned NV12 texture 是当前硬解稳定性的关键点：seek 或 pipeline recreate 后 FFmpeg decoder surface 可以被安全回收，不会被 Flutter/renderer 长时间引用。

## Headless / Flutter Texture

Flutter 主窗口使用 headless renderer，不直接 Present 到 SwapChain。`D3D11HeadlessOutput` 管理三缓冲 shared BGRA texture：

- native 持有 3 个 shared texture 和 handle。
- renderer 总是写入非 front 且 Flutter 未持有的 buffer。
- 绘制完成后切换 front handle，并通过 callback 通知 Flutter Texture 更新。
- resize 时旧 shared buffers 会延迟保活，避免 Flutter 仍在读取时被释放导致黑闪。

测试中的 `CAPTURE_VIEWPORT` 读取当前 front buffer，计算 hash、平均亮度和非黑像素占比，用于 UI 回归。

## 内存估算

| 资源 | 1080p 量级 |
|------|-----------|
| PacketQueue 100 slots | 约 1 MB，取决于压缩码率 |
| RGBA 帧 | 约 8 MB/帧 |
| NV12 帧 | 约 3 MB/帧 |
| Headless BGRA 三缓冲 | 约 24 MB |
| renderer-owned NV12 texture | 每轨约 3 MB，可随尺寸变化重建 |
