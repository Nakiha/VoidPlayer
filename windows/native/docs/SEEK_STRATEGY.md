# Seek 策略

## SeekController

头文件: `sync/seek_controller.h`

线程安全的 seek 请求队列，Renderer 写入，DemuxThread 消费。

```cpp
struct SeekRequest {
    int64_t target_pts_us;
    SeekType type;          // Keyframe 或 Exact
};

void request_seek(int64_t target_pts_us, SeekType type);
bool has_pending_seek() const;                // lock-free
optional<SeekRequest> take_pending();         // 原子取走
```

### 设计要点

- 只保留最新一次请求（覆盖旧请求）
- `has_pending_seek()` 无锁检查，DemuxThread 轮询无开销
- `take_pending()` 原子取走，保证只被消费一次

---

## Seek 类型

### I 帧 Seek (Keyframe)

```
av_seek_frame(fmt_ctx, stream_index, target_ts, AVSEEK_FLAG_BACKWARD)
```

- 定位到目标 PTS 之前最近的 I 帧
- 精度：1-2 秒误差（取决于 GOP 结构）
- 延迟：极低，无需额外解码
- 场景：拖拽进度条

### 精确 Seek (Exact)

```
av_seek_frame(..., AVSEEK_FLAG_BACKWARD)
→ 从 I 帧逐帧解码到 target_pts
→ 丢弃中间帧，保留目标帧
```

- 帧级精度
- 延迟：取决于 I 帧到目标帧的距离
- 场景：逐帧回退（缓存未命中时）

---

## Seek 触发矩阵

| 场景 | Seek 类型 | 缓冲处理 |
|------|----------|---------|
| 正常播放前进 | 无 seek | advance() |
| 逐帧前进 | 无 seek | advance() |
| 逐帧回退（缓存命中） | 无 seek | retreat() |
| 逐帧回退（缓存未命中） | Exact | 清空 + 重填充 |
| 拖拽进度条 | Keyframe | 清空 + 重填充 |
| 倍速切换 | 无 seek | Clock 调整 |

---

## Seek 协调流

```
Renderer.seek(target, type)
  │
  ├─▶ SeekController.request_seek(target, type)
  │
  ├─▶ Clock.seek(target)               // 立即更新时钟
  │
  └─▶ DecodeThread.notify_seek(target)  // 通知解码线程

DemuxThread (轮询)
  └─▶ seek_ctrl_.take_pending()
      ├─▶ av_seek_frame(...)
      └─▶ packet_queue_.flush()

DecodeThread (收到 notify)
  ├─▶ 排空 PacketQueue 旧 packet
  ├─▶ track_buffer_.clear_frames()
  └─▶ 等待 Demux 提供新 packet → 解码到目标 PTS
```

---

## 边界条件

| 情况 | 处理 |
|------|------|
| Seek 到文件头 | av_seek_frame 自动定位到首帧 |
| Seek 到文件尾 | Demux EOF → Decode EOF → TrackBuffer Flushing |
| 连续快速 seek | SeekController 覆盖旧请求，只执行最新一次 |
| Seek 中再次 seek | 同上，不会堆积 |
| 硬解状态 seek | 重新发送 sequence header，flush decoder |
