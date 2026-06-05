# Seek 策略

## SeekController

头文件: `media/seek_controller.h`

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

Exact seek 仍依赖容器和码流在 seek 点之后提供足够的解码上下文。它不是对所有损坏或省略参数集的 bitstream 的修复器。

### H.264 / FLV SPS/PPS 限制

H.264 FLV 常见两种打包方式：

- SPS/PPS 写在 AVC sequence header，并且关键帧 IDR 不重复携带参数集。
- 每个关键帧附近重复 SPS/PPS，decoder 从 seek 点开始也能重新获得参数集。

当前 exact seek 对 H.264/FLV 是 best-effort。如果文件只在 AVC sequence header 里保存 SPS/PPS，直接 seek 到后续 IDR 可能让 decoder 在 flush 后缺少参数集，从而出现坏帧、黑帧或无法稳定输出暂停预览。Renderer 会记录 warning，但不会自动注入 SPS/PPS，也不会把 exact seek 静默降级成 keyframe seek。

需要 frame-accurate 暂停预览的 H.264/FLV 输入应在生成或 remux 时让关键帧重复 headers，例如 x264 `repeat-headers=1`，或转成参数集分布更适合随机访问的容器/码流。

行为变更必须满足以下约束：

- demux/bitstream 层先检测目标 IDR 附近是否已有 SPS/PPS。
- 对缺失参数集的 H.264/FLV 返回显式 unsupported-path status，或在 UI 层降级提示。
- 自动注入保存的 SPS/PPS 需要先证明不会破坏其他 seek/decode 路径。

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
python dev.py ui-test ui_tests/h265_seek_visual_regression.csv
```

H.264/FLV “IDR 不重复 SPS/PPS”的 exact seek 目前缺少可签入的小型回归资产。新增行为变更时需要补一个合法生成的 fixture，或者在 UI 自动化中提供一个可复现的本地样本路径并在最终说明里标注该覆盖不是通用 CI 用例。
