# 线程模型

## 线程角色

每路视频轨道拥有独立的 Demux + Decode 线程对，渲染线程为全局单例。

```
┌──────────────────────────────────────────────────────────┐
│                    Renderer (主线程)                       │
│  initialize() / shutdown() / play() / seek() / ...       │
└──────────────────────┬───────────────────────────────────┘
                       │ 启动/停止
       ┌───────────────┼───────────────┐
       ▼               ▼               ▼
┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│  Track 0     │ │  Track 1     │ │  Track N     │
│              │ │              │ │              │
│ DemuxThread  │ │ DemuxThread  │ │ DemuxThread  │
│ DecodeThread │ │ DecodeThread │ │ DecodeThread │
└──────────────┘ └──────────────┘ └──────────────┘
                       │
                       ▼
              ┌──────────────┐
              │ RenderThread │  单例，独占 D3D11 Context
              └──────────────┘
```

## 三类线程

### Demux 线程（每轨道一个）

- 从视频文件读取 AVPacket
- 过滤非视频 stream，PTS 转微秒
- 写入 PacketQueue（阻塞式有界队列）
- 轮询 SeekController 处理 pending seek

### Decode 线程（每轨道一个）

- 从 PacketQueue 消费 AVPacket
- FFmpeg 解码（D3D11VA 硬解 / 软解）
- FrameConverter 统一输出 TextureFrame
- 写入 TrackBuffer（BidiRingBuffer）
- 硬解时通过 shared_mutex 序列化 D3D11 Context 访问

### Render 线程（单例）

- 独占 D3D11 Immediate Context
- 从 Clock 获取 current_pts_us
- RenderSink 评估上屏决策
- 从各 TrackBuffer peek 帧
- PTS 对齐 → 合成 → Present
- Deadline-based sleep 避免漂移

## 渲染循环

```
while (running) {
    current_pts = clock_.current_pts_us();
    decision = render_sink_.evaluate();   // 对齐所有轨道

    if (decision.should_present) {
        composite(decision.frames);       // 着色器合成
        device_.present(sync_interval);   // vsync
    }

    // Deadline sleep: 计算下一帧的绝对 PTS，sleep 到该时刻
    next_pts = current_pts + frame_duration / speed;
    sleep_until(next_pts);
}
```

Deadline-based sleep 保证长时间播放无累积漂移。

## 锁策略

| 资源 | 保护方式 | 持锁时间 |
|------|---------|---------|
| PacketQueue | mutex + condvar | push/pop 瞬间 |
| BidiRingBuffer | mutex | push/peek/advance 瞬间 |
| Clock | mutable mutex | 查询/更新瞬间 |
| SeekController | mutex + atomic | request/take 瞬间 |
| D3D11 Context | recursive_mutex（硬解时共享） | decode 期间 |
| TrackBuffer state | mutex | 状态变更瞬间 |

## 线程间通信

| 通信方向 | 机制 | 用途 |
|---------|------|------|
| Renderer → DemuxThread | SeekController | 下发 seek 请求 |
| Renderer → DecodeThread | notify_seek() | 通知 seek，丢弃旧帧 |
| DemuxThread → DecodeThread | PacketQueue | AVPacket 传递 |
| DecodeThread → RenderThread | TrackBuffer | TextureFrame 传递 |
| PacketQueue | EOF signal | Demux 结束通知 |
| PacketQueue | abort() | 强制停止 |

## 启停顺序

```
启动: initialize()
  1. D3D11Device::initialize()      # GPU 初始化
  2. 各 TrackPipeline 构建          # Queue + Buffer
  3. DemuxThread::start()           # 开始填充 PacketQueue
  4. DecodeThread::start()          # 等待 Preroll 完成
  5. RenderThread 启动              # 开始上屏循环

停止: shutdown()
  1. RenderThread stop              # 退出渲染循环
  2. DecodeThread::stop()           # 停止解码
  3. PacketQueue::abort()           # 解除阻塞
  4. DemuxThread::stop()            # 停止读取
  5. D3D11Device::shutdown()        # 释放 GPU 资源
```
