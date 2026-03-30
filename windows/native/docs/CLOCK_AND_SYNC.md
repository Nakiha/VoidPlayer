# 时钟与同步

## Clock 类

头文件: `video_renderer/clock.h`

Clock 维护一个基于 base-time/base-pts 的时钟，支持暂停/恢复/倍速/Seek。

### 可注入时间源

```cpp
using TimeSource = std::function<int64_t()>;
Clock(TimeSource time_source = nullptr);  // nullptr = 系统时钟
```

测试时注入固定时间源，避免真实等待。

### 状态

| 字段 | 类型 | 初始值 | 说明 |
|------|------|--------|------|
| base_time_us_ | int64_t | 0 | 播放起始系统时间 |
| base_pts_us_ | int64_t | 0 | 起始 PTS |
| pause_time_us_ | int64_t | 0 | 暂停时刻 |
| speed_ | double | 1.0 | 倍速 |
| paused_ | bool | true | 初始暂停 |

### 核心 PTS 计算

```
current_pts_us() = base_pts_us_ + (now_us - base_time_us_) * speed_
                   （仅非暂停时）
暂停时返回 base_pts_us_
```

## 操作语义

### play()

```
base_time_us_ = now()
base_pts_us_  = 0
paused_       = false
```

### pause()

```
pause_time_us_ = now()
paused_        = true
```

### resume()

```
base_time_us_ += (now() - pause_time_us_)   // 补偿暂停时长
paused_        = false
```

### seek(target)

```
base_pts_us_  = target
base_time_us_ = now()
```

### set_speed(new_speed)

保持 current_pts_us 不跳变：

```
current = base_pts_us_ + (now - base_time_us_) * old_speed
base_time_us_ = now - (current - base_pts_us_) / new_speed
speed_ = new_speed
```

## A/V 同步：Deadline-Based Sleep

渲染循环不使用固定帧间隔 sleep，而是计算下一帧的**绝对 PTS 目标时间**。

```
render_loop:
    pts = clock_.current_pts_us()
    decision = render_sink_.evaluate()

    if decision.should_present:
        composite_and_present(decision.frames)

    // 计算下一帧的绝对 PTS
    next_pts = pts + frame_duration_us / speed
    // 转换为系统时间目标
    target_time = base_time_us + (next_pts - base_pts_us) / speed
    sleep_until(target_time)
```

优势：
- 长时间播放无累积漂移
- 倍速切换即时生效
- 暂停后恢复不跳帧

## 多轨道 PTS 对齐

RenderSink 负责多轨道帧选择：

1. 对每个 TrackBuffer 调用 peek(0)
2. 丢弃过期帧（pts + duration < current_pts）
3. 如果当前帧未到，返回 should_present = false
4. 所有轨道就绪后返回 PresentDecision

### 容差

```cpp
constexpr int64_t PTS_TOLERANCE_US = 5000;  // 5ms
```

### PresentDecision

```cpp
struct PresentDecision {
    bool should_present;                            // 是否上屏
    vector<optional<TextureFrame>> frames;          // 各轨道选中帧
    int64_t current_pts_us;                         // 当前时钟 PTS
};
```
