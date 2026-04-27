# 独立轨道上屏 (Independent Track Presentation)

## 问题

多轨道播放时，`RenderSink::evaluate()` 采用 **all-or-nothing** 策略：仅当所有轨道都有有效帧时才上屏 (`should_present = all_ready`)。

当两个视频的 PTS 边界不对齐（例如 H.264 的 time_base=1/90000 和 H.265 的 time_base=1/1200000），会出现周期性阻塞：

```
clock=34ms:  track[0] pts=33ms ✓ (窗口内)  track[1] pts=50ms ✗ (未来帧)
→ BLOCKED → 等到 clock=50ms → track[0] 的 33ms 帧已过期被丢弃
→ track[1] 每 8 帧多丢 1 帧 → 视觉上 60fps→30fps
```

日志特征：`track[1] expired 2 frame(s)` 周期性出现，`BLOCKED reason=future_frame`。

## 方案

### 核心改动：any-ready 替代 all-ready

每个轨道独立上屏，互不阻塞。只要有一个轨道有有效帧就上屏。缺帧的轨道沿用上次显示的帧。

### 涉及文件

| 文件 | 改动 |
|------|------|
| `sync/render_sink.cpp` | `all_ready` → `any_ready`，选中帧时设 `any_ready = true` |
| `renderer.cpp` render_loop | `present_frame()` 前从 `last_decision_` 填充缺帧轨道 |
| `tests/renderer/test_render_sink.cpp` | "outside tolerance" 测试改为验证独立上屏行为 |

### render_sink.cpp 改动

```cpp
// Before: all_ready = true; ... decision.should_present = all_ready;
// After:
bool any_ready = false;
for (size_t t = 0; t < tracks_.size(); ++t) {
    // ... 帧选择逻辑不变 ...
    if (/* frame selected */) {
        decision.frames[t] = frame;
        any_ready = true;           // ← 只要有一个轨道就绪就上屏
    } else {
        decision.frames[t] = std::nullopt;  // ← 缺帧标记
    }
}
decision.should_present = any_ready;
```

### renderer.cpp render_loop 改动

```cpp
if (decision.should_present) {
    // 独立上屏：缺帧轨道沿用上次显示的帧
    for (size_t i = 0; i < decision.frames.size(); ++i) {
        if (!decision.frames[i].has_value() &&
            i < last_decision_.frames.size() &&
            last_decision_.frames[i].has_value()) {
            decision.frames[i] = last_decision_.frames[i];
        }
    }
    present_frame(decision);
    last_decision_ = decision;
}
```

`draw_frame()` 已有 `if (!frames[i].has_value()) continue` 逻辑，无需改动。

### 不需要改动的部分

| 组件 | 原因 |
|------|------|
| `draw_frame()` | 已处理 `std::nullopt`（跳过该轨道） |
| sleep 逻辑 | 基于 PTS 绝对时间计算唤醒，与 all/any 无关 |
| `draw_paused_frame()` | 已使用 `last_decision_` 做回退 |
| `redraw_layout()` | 仅在布局变化时重绘 |
| TrackBuffer | peek/advance 语义不变 |
| step_forward/step_backward | 暂停态操作，不经过 evaluate |

## 验证

```bash
# 构建
python dev.py build --native

# 双视频测试 — 观察日志不再出现 BLOCKED 和 expired 2 frame(s)
python dev.py demo --log-level debug \
  resources/video/h264_9s_1920x1080.mp4 \
  resources/video/h265_10s_1920x1080.mp4

# 单视频回归 — 不应该有行为变化
python dev.py demo --log-level debug \
  resources/video/h264_9s_1920x1080.mp4

# 运行测试
python dev.py test
```

## 预期效果

- `BLOCKED` 日志消失（仅在所有轨道都无帧时才 BLOCKED）
- 不再出现 `expired 2 frame(s)`，每轨每次最多过期 1 帧
- 双 60fps 视频同时播放，均恢复 60fps 流畅度
- 单视频播放无行为变化
