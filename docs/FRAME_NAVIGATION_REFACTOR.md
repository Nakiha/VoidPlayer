# 双向帧队列重构设计文档

## 背景

当前 `PREV_FRAME` 和 `NEXT_FRAME` 实现存在问题：

| 操作 | 当前实现 | 问题 |
|------|----------|------|
| **NEXT_FRAME** | `request_all_frames()` 直接解码下一帧 | 正常工作 |
| **PREV_FRAME** | `seek_to_precise(current_pts - 1ms)` | 不正确，帧间隔通常是 16-40ms，-1ms 无法回退 |

## 业界方案对比

| 方案 | 代表软件 | 复杂度 | 内存占用 | 随机访问 |
|------|----------|--------|----------|----------|
| 双向帧队列 | mpv, VLC | 高 | 中（固定帧数） | 仅队列范围 |
| 帧索引 + seek | PotPlayer | 中 | 低（仅 PTS） | 任意位置 |
| 历史帧缓存 | MPC-HC | 低 | 低 | 仅后退 |

**推荐方案**：双向帧队列（与现有架构最契合）

---

## 现有架构分析

```
┌─────────────────────────────────────────────────────────────┐
│                      Python Layer                           │
│  ┌─────────────┐    ┌─────────────┐    ┌────────────────┐  │
│  │DecoderPool  │───>│ DecodeWorker│───>│ FrameQueue(12) │  │
│  └─────────────┘    └─────────────┘    └────────────────┘  │
│                            │                     │          │
│                            ▼                     ▼          │
│                    ┌─────────────┐         pop_frame()      │
│                    │HardwareDecoder         set_pending()   │
│                    └─────────────┘                           │
└─────────────────────────────────────────────────────────────┘

当前 FrameQueue：
  [Frame0] -> [Frame1] -> [Frame2] -> ... -> [Frame11]
     ↑
   push()                              pop() →
     ↑                                    ↑
   解码器                               渲染器

问题：
1. 单向队列，无法向前访问
2. seek 时清空队列，丢失历史帧
3. 无法实现 PREV_FRAME
```

---

## 新架构设计

### 1. 双向帧队列 (BidiFrameQueue)

```cpp
class BidiFrameQueue {
public:
    explicit BidiFrameQueue(size_t history_size = 4, size_t future_size = 12);

    // 向前填充（正常播放方向）
    // 若 future_ 已满，丢弃最老的帧（pop_back）后再 push
    bool push_future(AVFrame* frame);

    // 向后填充（用于 prev_frame 缓存）
    // 若 history_ 已满，丢弃最老的帧（pop_front）后再 push
    void push_history(AVFrame* frame);

    // 获取当前帧（不移除）
    AVFrame* current();

    // 移动指针
    bool move_next();  // 前进一帧：current_ 移入 history_，从 future_ 取新帧
    bool move_prev();  // 后退一帧：current_ 移入 future_ 头部，从 history_ 取新帧

    // 状态查询
    size_t history_size() const;   // 历史帧数量
    size_t future_size() const;    // 未来帧数量
    int64_t current_pts_ms() const;

    // Seek 时清空
    void clear();

    // 获取指定方向的帧 PTS（用于 UI 显示）
    std::vector<int64_t> get_history_pts() const;
    std::vector<int64_t> get_future_pts() const;

private:
    // 环形缓冲区或 deque 实现
    std::deque<FrameHolder> history_;  // 已播放帧（前4帧）
    FrameHolder current_;               // 当前帧
    std::deque<FrameHolder> future_;   // 预解码帧（后12帧）

    size_t max_history_;
    size_t max_future_;
};
```

**队列结构**：

```
           history      current      future
        ┌─────────┬───────────┬──────────────┐
        │ -4 -3 -2 -1 │  0  │ +1 +2 +3 ... +12 │
        └─────────┴───────────┴──────────────┘
              ▲                    ▲
           move_prev()         move_next()
           (O(1)直接取)        (O(1)直接取)
```

### 2. 修改 DecodeWorker

```cpp
class DecodeWorker {
public:
    // 新增：精确帧导航
    bool prev_frame();  // 后退一帧，从 history 取
    bool next_frame();  // 前进一帧，从 future 取

    // 新增：获取帧队列状态
    size_t history_size() const;
    size_t future_size() const;

    // 修改：seek 后需要重建帧缓冲
    void seek_precise(int64_t timestamp_ms);

private:
    BidiFrameQueue frame_queue_{4, 12};  // 前4帧，后12帧

    // 新增：seek 后向后填充历史帧
    void fill_history_backward(int64_t target_pts);

    // 新增：重建帧缓冲（seek 后调用）
    void rebuild_frame_buffer(int64_t target_pts);
};
```

### 3. PREV_FRAME 实现策略

**策略 A：仅使用 history 缓冲区（简单）**
- 后退时直接从 history 取帧
- 如果 history 为空，无法后退（或触发 seek 重建）

**策略 B：动态重建（完整）**
- 后退时优先从 history 取
- history 不足时，seek 到目标帧附近重新解码填充

**推荐：策略 A + 边界处理**
- 正常情况下从 history 取（O(1)）
- history 为空时，seek 到 `current_pts - 帧间隔 * 4` 并重建

---

## 需要修改的文件

### Native 层 (C++)

| 文件 | 修改内容 |
|------|----------|
| `frame_queue.hpp` | 新增 `BidiFrameQueue` 类，或重构现有 `FrameQueue` |
| `frame_queue.cpp` | 实现双向队列逻辑 |
| `decode_worker.hpp` | 新增 `prev_frame()`, `next_frame()` 方法 |
| `decode_worker.cpp` | 实现帧导航逻辑，修改 seek 后的缓冲重建 |
| `bindings.cpp` | 暴露新 API 给 Python |

### Python 层

| 文件 | 修改内容 |
|------|----------|
| `decoder_pool.py` | 修改 `prev_frame()`, `next_frame()` 调用新 API |

---

## 详细流程

### 正常播放流程（无变化）

```
Timer tick
    │
    ▼
DecodeWorker.pop_frame() 即 move_next()
    │
    ├─ future 不为空 ─> 1. current_ 移入 history_（若满则丢弃最老帧）
    │                    2. 从 future_ 头部取帧作为新 current_
    │                    3. 若 future_ 不足，触发填充
    │
    └─ future 为空 ─> 触发 FILL_BUFFER，等待解码
```

### NEXT_FRAME 流程

```
next_frame()
    │
    ▼
DecodeWorker.move_next()
    │
    ├─ future 不为空 ─> 1. current_ 移入 history_
    │                    （若 history_ 满则先 pop_front 丢弃最老帧）
    │                    2. 从 future_ 头部取帧作为新 current_
    │                    3. 若 future_ 不足，触发填充
    │
    └─ future 为空 ─> 解码新帧
```

### PREV_FRAME 流程

```
prev_frame()
    │
    ▼
DecodeWorker.move_prev()
    │
    ├─ history 不为空 ─> 1. current_ 移入 future_ 头部
    │                     （若 future_ 满则先 pop_back 丢弃尾部帧）
    │                     2. 从 history_ 尾部取帧作为新 current_
    │
    └─ history 为空 ─> seek 到 current_pts - interval * 4
                       重新解码填充 history + current + future
```

### Seek 后重建流程

```
seek_precise(target_pts)
    │
    ▼
1. 清空 history, future, current
    │
    ▼
2. seek_to_precise_internal(target_pts)
    │
    ▼
3. 解码当前帧 -> current
    │
    ▼
4. 向前解码 12 帧 -> future
    │
    ▼
5.（可选）向后填充 history
   - 需要从最近关键帧重新解码
   - 或者 history 保持为空，用户后退时按需填充
```

---

## 关键问题

### Q1: 如何填充 history？

**方案 1：不主动填充，按需重建**
- Seek 后 history 为空
- 用户后退时，如果 history 为空则 seek 重建
- 优点：简单，不增加 seek 延迟
- 缺点：首次后退可能较慢

**方案 2：Seek 后异步填充**
- Seek 后在后台线程填充 history
- 需要从当前帧之前的关键帧开始解码
- 优点：后退响应快
- 缺点：实现复杂，需要额外 seek

**推荐：方案 1**（先实现简单方案）

### Q2: 帧数据如何管理？

当前使用 `AVFrame*`，需要考虑：
- 引用计数：`av_frame_ref()` 增加引用
- 释放：`av_frame_free()` 释放

**建议**：使用 RAII 包装器 `FrameHolder`，队列使用 `std::deque<FrameHolder>` 而非裸指针

```cpp
class FrameHolder {
public:
    explicit FrameHolder(AVFrame* frame = nullptr) : frame_(frame) {}
    ~FrameHolder() { if (frame_) av_frame_free(&frame_); }

    // 禁止拷贝
    FrameHolder(const FrameHolder&) = delete;
    FrameHolder& operator=(const FrameHolder&) = delete;

    // 移动构造
    FrameHolder(FrameHolder&& other) noexcept : frame_(other.frame_) {
        other.frame_ = nullptr;
    }

    // 移动赋值
    FrameHolder& operator=(FrameHolder&& other) noexcept {
        if (this != &other) {
            if (frame_) av_frame_free(&frame_);
            frame_ = other.frame_;
            other.frame_ = nullptr;
        }
        return *this;
    }

    AVFrame* get() const { return frame_; }
    AVFrame* release() {
        auto* f = frame_;
        frame_ = nullptr;
        return f;
    }

private:
    AVFrame* frame_;
};
```

### Q3: 多轨道同步？

当前架构支持多轨道，需要考虑：
- 每个轨道独立的双向帧队列
- PREV_FRAME/NEXT_FRAME 需要同步所有轨道

**实现**：
```python
# decoder_pool.py
def prev_frame(self):
    """所有轨道同时后退一帧"""
    for track in self._tracks:
        if track and track.enabled and track.worker:
            track.worker.prev_frame()

def next_frame(self):
    """所有轨道同时前进一帧"""
    for track in self._tracks:
        if track and track.enabled and track.worker:
            track.worker.next_frame()
```

---

## 实现步骤

### Phase 1: BidiFrameQueue 基础实现
1. 创建 `BidiFrameQueue` 类
2. 实现 `push_future()`, `push_history()`, `current()`
3. 实现 `move_next()`, `move_prev()`
4. 单元测试

### Phase 2: 集成到 DecodeWorker
1. 替换 `FrameQueue` 为 `BidiFrameQueue`
2. 修改 `fill_frame_buffer()` 填充 future
3. 实现 `prev_frame()`, `next_frame()`
4. 修改 seek 后的缓冲清空逻辑

### Phase 3: Python 层集成
1. 更新 `bindings.cpp` 暴露新 API
2. 修改 `decoder_pool.py` 的帧导航方法
3. 测试验证

### Phase 4: 优化（可选）
1. Seek 后异步填充 history
2. 帧队列状态 UI 显示
3. 边界情况处理（视频开头/结尾）

---

## 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 内存增加 | 4 帧历史帧 | 可配置 history_size |
| 首次后退慢 | 用户体验 | 显示加载提示 |
| 多轨道不同步 | 画面错位 | 等待所有轨道就绪 |
| B 帧依赖 | 解码错误 | 确保从关键帧开始解码 |

---

## 参考

- mpv 源码：`video/decode/vd_lavc.c` - 帧缓冲管理
- VLC 源码：`src/input/decoder.c` - 解码器队列
- FFmpeg：`avcodec_send_packet/receive_frame` - 解码 API
