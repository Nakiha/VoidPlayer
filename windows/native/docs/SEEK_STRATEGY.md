# Seek 策略

## SeekController

头文件: `sync/seek_controller.h`

Renderer 写入 seek 请求，DemuxThread 消费。

```cpp
struct SeekRequest {
    int64_t target_pts_us;
    SeekType type; // Keyframe 或 Exact
};
```

设计点：

- 只保留最新请求，连续 seek 不排队。
- DemuxThread 轮询并执行 `av_seek_frame`。
- packet queue 在 seek 时 flush，避免旧 packet 混入新位置。

## Seek 类型

### Keyframe

```
av_seek_frame(..., AVSEEK_FLAG_BACKWARD)
```

定位到目标之前最近关键帧。延迟低，但暂停预览可能显示关键帧或关键帧后的较早帧。

### Exact

```
seek 到关键帧 -> 解码并丢弃目标前帧 -> 发布目标前最后可显示帧
```

用于逐帧回退缓存未命中，以及需要接近用户点击 PTS 的暂停预览。B 帧/DPB 可能导致输出顺序晚于输入，因此 DecodeThread 有 exact seek candidate/reorder 逻辑。

## 当前触发矩阵

| 场景 | Seek 类型 | 说明 |
|------|----------|------|
| 播放中拖动进度条 | Keyframe | 优先低延迟，播放会自然追到目标附近 |
| 暂停时点击进度条 | Exact | 目标是稳定显示目标前最后一帧 |
| 逐帧前进 | 无 seek | 从 TrackBuffer 前进 |
| 逐帧回退缓存命中 | 无 seek | `BidiRingBuffer::retreat()` |
| 逐帧回退缓存未命中 | Exact | 回 seek 后重建预览 |
| 新增轨道 | Keyframe 到当前 clock | 让新轨和已有轨对齐 |

## HEVC 硬解 seek 稳定性

HEVC D3D11VA 在暂停状态下快速/连续 exact seek 对驱动非常敏感。当前策略：

- paused HEVC seek 可以延迟合并，避免连点时多个 seek 同时冲进 decoder。
- 必要时 recreate pipeline，让新的 codec/context 从干净状态处理 seek。
- seek preview ready 后设置短 settle window。
- exact seek 时轻微 pacing packet feeding，模拟播放态消费节奏。
- renderer 复制 NV12 slice 到自有 texture，避免 seek/recreate 后继续引用 decoder surface。

这套策略的目标是保留硬解性能，同时避免“一点进度条就崩”的驱动状态损坏。

## Seek 协调流

```
Renderer.seek(target, type)
  -> Clock.seek(target)
  -> optional HEVC paused-seek defer/recreate
  -> SeekController.request_seek(target, type)
  -> DecodeThread.notify_seek(target, type)

DemuxThread
  -> take_pending()
  -> av_seek_frame()
  -> packet_queue.flush()
  -> push fresh packets

DecodeThread
  -> clear old frames
  -> flush codec unless fresh codec
  -> exact discard/reorder if needed
  -> post-seek preroll
  -> TrackBuffer Ready
```

## 自动化验证

Seek 相关 native 改动至少跑 `python dev.py test`。如果影响主窗口上屏，补跑对应 UI 脚本，例如：

```bash
python dev.py ui-test test_scripts/h265_seek_visual_regression.csv
```
