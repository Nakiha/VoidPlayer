# Integration - 集成与同步设计文档

> **版本**: 1.0
> **状态**: 设计阶段
> **依赖**: 本项目所有模块

---

## 1. 模块概述

### 1.1 职责定义

| 组件 | 职责 |
|------|------|
| SyncController | 多视频时间同步、偏移管理 |
| PlaybackManager | 播放状态、解码器协调 |
| 外部接口 | 供 VoidView 主应用调用 |

### 1.2 模块关系

```
┌─────────────────────────────────────────────────────────┐
│                    PlayerWindow                          │
│  ┌─────────────────────────────────────────────────┐    │
│  │               PlaybackManager                     │    │
│  │  ┌───────────────────────────────────────────┐  │    │
│  │  │            SyncController                  │  │    │
│  │  │  ┌─────────┐ ┌─────────┐ ┌─────────┐     │  │    │
│  │  │  │Decoder A│ │Decoder B│ │Decoder C│     │  │    │
│  │  │  └─────────┘ └─────────┘ └─────────┘     │  │    │
│  │  └───────────────────────────────────────────┘  │    │
│  └─────────────────────────────────────────────────┘    │
│         │                              │                │
│         ▼                              ▼                │
│    ControlPanel                    GLWidget             │
└─────────────────────────────────────────────────────────┘
```

---

## 2. 时间同步设计

### 2.1 同步原理

```
虚拟主时钟 (Master Clock)
        │
        ├── offset[0] = 0ms    ──▶ 视频 A: pts = master + 0
        ├── offset[1] = +500ms ──▶ 视频 B: pts = master + 500
        └── offset[2] = -200ms ──▶ 视频 C: pts = master - 200
```

### 2.2 SyncController 接口

| 方法 | 说明 |
|------|------|
| `add_source(index, decoder)` | 添加视频源 |
| `set_offset(index, offset_ms)` | 设置时间偏移 |
| `play()` / `pause()` / `stop()` | 播放控制 |
| `seek_to(timestamp_ms)` | 跳转 |
| `tick()` | 时钟推进 (定时器调用) |

**属性**:
| 属性 | 类型 | 说明 |
|------|------|------|
| `current_time_ms` | int | 当前主时钟值 |
| `duration_ms` | int | 最长视频时长 |
| `is_playing` | bool | 是否播放中 |

### 2.3 帧同步流程

```
tick() 被调用
    │
    ▼
更新主时钟 = elapsed_time
    │
    ▼
遍历所有源:
    │
    ├── 计算 target_pts = master_clock + offset[i]
    │
    ├── if last_pts < target_pts:
    │       decode_next_frame()
    │
    └── 更新 last_pts
    │
    ▼
返回是否有新帧
```

---

## 3. PlaybackManager 设计

### 3.1 职责

- 协调 SyncController 和解码器
- 管理播放定时器
- 提供 Qt 信号给 UI

### 3.2 信号定义

| 信号 | 参数 | 触发时机 |
|------|------|----------|
| `frame_ready` | 无 | 新帧解码完成 |
| `time_changed` | int (ms) | 主时钟更新 |
| `duration_changed` | int (ms) | 源加载完成 |
| `state_changed` | PlayState | 播放状态变化 |
| `source_loaded` | (int, str) | 源加载成功 |
| `source_error` | (int, str) | 源加载失败 |
| `eof_reached` | 无 | 播放结束 |

### 3.3 状态机

```
         play()
STOPPED ──────────▶ PLAYING
    ▲                  │
    │           pause()│
    │                  ▼
    └──────────── PAUSED
         stop()
```

---

## 4. 外部调用接口

### 4.1 PlayerWindow 公共 API

```python
class PlayerWindow:
    # 加载源
    def load_sources(sources: list[str]) -> bool

    # 播放控制
    def play() -> None
    def pause() -> None
    def seek_to(timestamp_ms: int) -> None

    # 同步设置
    def set_sync_offset(index: int, offset_ms: int) -> None

    # 视图模式
    def set_view_mode(mode: str) -> None  # "side_by_side" / "split_screen"
```

### 4.2 使用示例

```python
# 基本使用
player = PlayerWindow()
player.load_sources([
    "original.mp4",
    "encoded.mp4"
])
player.show()

# 带时间偏移
player.load_sources(["ref.mp4", "test.mp4"])
player.set_sync_offset(1, 500)  # test.mp4 延后 500ms
player.set_view_mode("split_screen")
player.play()
```

### 4.3 嵌入到其他窗口

```python
class MainWindow(QMainWindow):
    def __init__(self):
        self.player = PlayerWindow(self)
        self.setCentralWidget(self.player)
```

---

## 5. 播放控制面板交互

### 5.1 信号流

```
ControlPanel                 PlayerWindow
    │                              │
    │ play_clicked                 │
    ├──────────────────────────────▶│
    │                              │
    │                     play()    │
    │                              ▼
    │                        PlaybackManager.play()
    │                              │
    │                              ▼
    │                        state_changed(PLAYING)
    │                              │
    │◀──────────────────────────────┤
    │   set_playing(True)           │
```

### 5.2 时间轴交互

```
用户拖动滑块
    │
    ▼
seek_requested.emit(position_ms)
    │
    ▼
PlayerWindow.seek_to(position_ms)
    │
    ▼
PlaybackManager.seek_to()
    │
    ▼
SyncController.seek_to()
    │
    ▼
所有 Decoder.seek_to()
    │
    ▼
frame_ready.emit() ──▶ GLWidget.update()
```

---

## 6. 性能优化要点

### 6.1 解码优化

| 优化项 | 说明 |
|--------|------|
| 帧预取 | 提前解码若干帧缓存 |
| 线程池 | 多视频并行解码 |
| 帧队列 | 解码与渲染解耦 |

### 6.2 渲染优化

| 优化项 | 说明 |
|--------|------|
| VSync | 避免撕裂 |
| 按需重绘 | 只在帧变化时 update() |
| 双缓冲 | QOpenGLWidget 默认支持 |

---

## 7. 错误处理策略

### 7.1 解码错误

```
decode_next_frame() 失败
    │
    ▼
检查 has_error()
    │
    ├── True: 记录错误日志，标记该源异常
    │
    └── False (EOF): 正常结束
```

### 7.2 源加载错误

```
load_sources() 过程
    │
    ├── 适配器创建失败 → source_error 信号
    │
    ├── 验证失败 → source_error 信号
    │
    └── 解码器初始化失败 → source_error 信号
```

---

## 8. 目录结构

```
player/
├── __init__.py           # 模块导出
├── player_window.py      # 主窗口
├── gl_widget.py          # OpenGL 画布
├── control_panel.py      # 控制面板
├── view_mode.py          # ViewMode 枚举
├── sync_controller.py    # 时间同步
├── playback_manager.py   # 播放管理
└── adapters/             # 源适配器
    ├── __init__.py
    ├── base.py
    ├── registry.py
    ├── local_file.py
    ├── http_source.py
    └── stream.py
```

---

## 9. 验收标准

- [ ] 多视频同步播放，时间戳一致
- [ ] 时间偏移设置生效
- [ ] 播放/暂停/seek 响应正确
- [ ] 逐帧步进功能正常
- [ ] 外部 API 调用成功
- [ ] 播放结束正确触发 EOF
- [ ] 错误情况有明确反馈

---

## 10. 依赖关系

**前置**:
- [native-core.md](native-core.md) - C++ 解码器
- [source-adapter.md](source-adapter.md) - 源适配器
- [ui-layer.md](ui-layer.md) - UI 组件

**本文档为最终集成模块**
