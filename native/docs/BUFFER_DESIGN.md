# 缓冲设计

## 三层缓冲

```
DemuxThread → PacketQueue → DecodeThread → TrackBuffer → RenderThread
               (AVPacket)                    (TextureFrame)
```

---

## PacketQueue

头文件: `buffer/packet_queue.h`

有界阻塞队列，传递 AVPacket 所有权。

| 参数 | 值 |
|------|-----|
| 默认容量 | 100 |
| 元素类型 | `AVPacket*`（调用者持有所有权） |

### 操作

| 方法 | 行为 |
|------|------|
| `push(pkt)` | 阻塞写入（满时等待），转移所有权 |
| `pop()` | 阻塞读取（空时等待），获取所有权 |
| `try_pop()` | 非阻塞，无数据返回 nullptr |
| `flush()` | 丢弃所有已排队 packet |
| `abort()` | 解除所有阻塞等待 |
| `reset()` | abort 后恢复可用 |
| `signal_eof()` | 标记流结束 |
| `is_eof()` | 检查 EOF |

### 并发保护

mutex + 两个 condvar（full / empty），push 和 pop 互斥但快速。

---

## BidiRingBuffer

头文件: `buffer/bidi_ring_buffer.h`

双向环形缓冲，支持前进/后退 peek。

```
               write_idx (Decode 写入)
                   ↓
  ┌────┬────┬────┬────┬────┬────┐
  │B-2 │B-1 │ F0 │ F1 │ F2 │ F3 │
  └────┴────┴────┴────┴────┴────┘
              ↑
          read_idx (Render 读取)

  ← backward (2) →  ← forward (4) →
```

### 构造

```cpp
BidiRingBuffer(size_t forward_depth = 4, size_t backward_depth = 2);
// capacity = forward_depth + backward_depth
```

### 操作

| 方法 | 说明 |
|------|------|
| `push(frame)` | 写入，write_idx++，满时返回 false |
| `peek(offset)` | 查看 read_idx + offset 的帧（负数为后退） |
| `advance()` | read_idx++ |
| `retreat()` | read_idx--（最多退 backward_depth 帧） |
| `can_retreat()` | 是否可后退 |
| `clear()` | 重置所有指针 |

### 并发

单 mutex 保护，Decode push 和 Render peek/advance 互斥但窗口极小。

---

## TrackBuffer

头文件: `buffer/track_buffer.h`

TrackBuffer 封装 BidiRingBuffer + 状态机 + Preroll 机制。

### 状态机

```
┌───────┐    push_frame()    ┌──────────┐   has_preroll()   ┌───────┐
│ Empty │──────────────────▶ │ Buffering│─────────────────▶│ Ready │
└───────┘                    └──────────┘                   └───┬───┘
     ▲                           │                              │
     │                      set_state(Error)           EOF / stop
     │                           │                              │
     └───────────────────── ┌────▼────┐                         │
        clear_frames()      │  Error  │◀────────────────────────┘
                             └─────────┘     set_state(Error)
                                                  │
                             ┌──────────┐◀────────┘
                             │ Flushing │  EOF 后清空剩余帧
                             └──────────┘
```

### 状态说明

| 状态 | 含义 |
|------|------|
| Empty | 初始/清空后，无可用帧 |
| Buffering | 正在填充，尚未达到 Preroll 目标 |
| Ready | Preroll 完成，可供 Render 读取 |
| Flushing | EOF 后清空剩余帧 |
| Error | 出错 |

### Preroll 机制

播放开始前，TrackBuffer 需积累一定帧数才允许 Render 读取：

```cpp
// TrackBuffer 构造
TrackBuffer(size_t forward_depth = 4, size_t backward_depth = 2);

// Preroll 目标 = forward_depth
// has_preroll() = (total_count() >= preroll_target())
```

**为什么需要 Preroll**：避免播放首帧卡顿，确保 Render 启动时缓冲区已有足够帧。

### 默认缓冲深度

| 参数 | 值 | 说明 |
|------|-----|------|
| forward_depth | 4 | 前向缓冲 4 帧 |
| backward_depth | 2 | 后向缓冲 2 帧（逐帧回退） |
| 总容量 | 6 | forward + backward |
| Preroll 目标 | 4 | 等于 forward_depth |
| PacketQueue | 100 | AVPacket 缓冲 |
