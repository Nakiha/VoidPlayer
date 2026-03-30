# 数据管线

> 本文档描述帧数据从文件到屏幕的完整路径。

## 总览

```
┌─────────┐     ┌────────────┐     ┌─────────────┐
│  File    │────▶│  DemuxThread│────▶│ PacketQueue  │
│ (mkv/mp4)│     │  AVPacket   │     │ (100 slots)  │
└─────────┘     │  PTS=μs     │     └──────┬──────┘
                └────────────┘            │
                                          ▼
                ┌────────────┐     ┌─────────────┐
                │DecodeThread│◀────│ PacketQueue  │
                │  AVFrame   │     └─────────────┘
                └──────┬─────┘
                       │
                       ▼
                ┌──────────────┐     ┌──────────────────────┐
                │FrameConverter│────▶│    TextureFrame       │
                │              │     │  pts_us / duration_us │
                └──────────────┘     │  texture_handle       │
                                     │  is_nv12 / is_ref     │
                                     │  hw_frame_ref         │
                                     │  cpu_data             │
                                     └──────────┬───────────┘
                                                │
                       ▼
                ┌──────────────────┐     ┌─────────────┐
                │ BidiRingBuffer   │────▶│ RenderSink  │
                │ (TrackBuffer)    │     │ evaluate()  │
                └──────────────────┘     └──────┬──────┘
                                                │
                                                ▼
                                         ┌─────────────┐
                                         │ D3D11 Present│
                                         └─────────────┘
```

## 帧格式变迁

| 阶段 | 格式 | 说明 |
|------|------|------|
| Demux 输出 | AVPacket | 压缩数据，PTS 已转微秒 |
| Decode 输出 | AVFrame | YUV420P（软解）或 NV12（硬解 D3D11VA） |
| FrameConverter 输出 | TextureFrame | 统一封装，见下表 |

## TextureFrame 字段

```cpp
struct TextureFrame {
    int64_t pts_us;                       // 显示时间戳（微秒）
    int64_t duration_us;                  // 帧持续时间（微秒）
    int width, height;                    // 帧尺寸
    bool is_ref;                          // true = 引用硬解纹理，不持有
    void* texture_handle;                 // ID3D11Texture2D*（类型擦除）
    bool is_nv12;                         // true = NV12（硬解零拷贝）
    int texture_array_index;              // D3D11VA 纹理数组索引
    shared_ptr<void> hw_frame_ref;        // AVFrame 引用，防止 pool 回收
    shared_ptr<vector<uint8_t>> cpu_data; // 软解 RGBA 数据（上传前）
};
```

## 两条路径

### 软解路径（拷贝）

```
AVFrame(YUV420P) → sws_scale() → RGBA CPU buffer → D3D11 Upload → ID3D11Texture2D
```

- 每帧有一次 CPU→GPU 拷贝
- 适用于所有编码格式
- cpu_data 持有 RGBA 数据

### 硬解路径（零拷贝）

```
AVFrame(D3D11VA NV12) → 提取 texture 引用 → 直接创建 SRV → Shader NV12→RGB
```

- 无 CPU 拷贝，GPU 纹理直接绑定
- `is_nv12 = true`, `is_ref = true`
- `hw_frame_ref` 持有 AVFrame 引用防止 FFmpeg pool 回收
- Shader 内完成 BT.601 NV12→RGB 转换
- 需 `device_mutex` 序列化 D3D11 Context 访问

## PTS 变换

```
文件时间基 (AVStream.time_base, 如 1/30000)
  → av_rescale_q(pts, stream->time_base, {1, 1000000})
  → 微秒 (int64_t)
  → 全链路统一使用微秒
```

## 内存估算（1080p 单轨）

| 资源 | 大小 |
|------|------|
| PacketQueue（100 slots）| ~1 MB |
| 软解 RGBA 帧（6帧） | ~48 MB（8MB×6） |
| 硬解 NV12 帧（4帧） | ~24 MB（6MB×4） |
| D3D11 纹理池 | ~8 MB |
