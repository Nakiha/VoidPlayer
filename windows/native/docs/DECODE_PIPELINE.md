# 解码管线

## 管线组成

```
File → [DemuxThread] → PacketQueue → [DecodeThread] → TrackBuffer
```

## DemuxThread

头文件: `decode/demux_thread.h`

### 职责

- 打开视频文件（avformat_open_input）
- 查找视频 stream（AVMEDIA_TYPE_VIDEO）
- PTS 转微秒：`av_rescale_q(pts, time_base, {1,1000000})`
- 读取 AVPacket 写入 PacketQueue
- 轮询 SeekController 处理 pending seek

### 统计信息 (DemuxStats)

```cpp
struct DemuxStats {
    int64_t duration_us;      // 视频总时长
    int width, height;        // 分辨率
    AVCodecID codec_id;       // 编码格式
    AVRational time_base;     // 流时间基
    AVRational frame_rate;    // 帧率
};
```

### Seek 处理

DemuxThread 在每次循环检查 SeekController：

```
if (seek_ctrl_.has_pending_seek()) {
    req = seek_ctrl_.take_pending();
    av_seek_frame(..., req.target_pts_us, flags);
    packet_queue_.flush();       // 丢弃旧 packet
}
```

---

## DecodeThread

头文件: `decode/decode_thread.h`

### 职责

- 从 PacketQueue 消费 AVPacket
- 调用 FFmpeg 解码器（硬解/软解）
- 通过 FrameConverter 统一输出 TextureFrame
- 写入 TrackBuffer

### 硬件解码启用

```cpp
// 必须在 start() 之前调用
bool enable_hardware_decode(void* native_device,   // ID3D11Device*
                            std::recursive_mutex* device_mutex = nullptr);
```

失败时自动回退软解。

### Seek 协调

收到 `notify_seek()` 后：
1. 排空当前 PacketQueue 中的旧 packet
2. 丢弃所有已解码但 PTS 不匹配的帧
3. 从新位置继续解码

---

## FrameConverter

头文件: `decode/frame_converter.h`

根据初始化路径分为两条管线：

### 软件路径

```cpp
bool init_software(int src_w, int src_h, AVPixelFormat src_fmt);
// 内部创建 SwsContext (sws_scale)
```

```
AVFrame(YUV420P/etc) → sws_scale() → RGBA buffer → TextureFrame
```

### 硬件路径

```cpp
bool init_hardware(void* d3d_device, void* d3d_context,
                   int src_w, int src_h, HwDecodeType hw_type);
```

```
AVFrame(D3D11VA) → 提取 ID3D11Texture2D 引用 → TextureFrame(is_nv12=true)
```

无 CPU 拷贝。`hw_frame_ref` 持有 AVFrame 引用防止 pool 回收。

---

## HwDecodeProvider 接口

头文件: `decode/hw/hw_decode_provider.h`

抽象硬件解码提供者，支持扩展。

```cpp
class HwDecodeProvider {
    virtual bool probe(const AVCodec* codec) const = 0;
    virtual HwDecodeInitResult init(void* native_device, int w, int h,
                                     recursive_mutex* mutex) = 0;
    virtual void shutdown() = 0;
    virtual HwDecodeType type() const = 0;
};
```

### HwDecodeType 枚举

| 值 | 说明 |
|-----|------|
| None | 未启用 |
| D3D11VA | Windows D3D11 硬解（已实现） |
| CUDA | NVIDIA CUDA（预留） |
| DXVA2 | legacy（预留） |
| Vulkan | 跨平台（预留） |

### D3D11VAProvider

头文件: `decode/hw/d3d11va_provider.h`

当前唯一实现：
- `probe()` 检查 codec 是否支持 D3D11VA
- `init()` 创建 `AVBufferRef* hw_device_ctx`
- 线程安全：通过 `recursive_mutex` 序列化 D3D11 访问

### 工厂函数

```cpp
HwDecodeInitResult try_hw_decode_providers(
    void* native_device, const AVCodec* codec,
    int w, int h, recursive_mutex* mutex);
```

按优先级尝试所有已注册 Provider，失败返回 `{success=false}`。
