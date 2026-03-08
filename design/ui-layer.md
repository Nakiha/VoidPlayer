# UI Layer - Python UI 层设计文档

> **版本**: 1.0
> **状态**: 设计阶段
> **依赖**: PySide6, qfluentwidgets, PyOpenGL

---

## 1. 模块概述

### 1.1 职责定义

| 组件 | 职责 |
|------|------|
| PlayerWindow | 主窗口、导航、布局 |
| GLWidget | OpenGL 渲染画布、着色器 |
| ControlPanel | 播放控制、时间轴 |

### 1.2 窗口结构

```
┌─────────────────────────────────────────────────────────────┐
│                   PlayerWindow (FluentWindow)                │
├────────────┬────────────────────────────────────────────────┤
│            │                                                │
│ Navigation │                GLWidget                        │
│    Bar     │           (QOpenGLWidget)                      │
│            │                                                │
│ ┌────────┐ │     ┌──────────────┬──────────────┐           │
│ │Side-by │ │     │    视频 A    │    视频 B    │           │
│ │Side    │ │     │              │              │           │
│ │Split   │ │     │              │              │           │
│ └────────┘ │     └──────────────┴──────────────┘           │
│            ├────────────────────────────────────────────────┤
│            │               ControlPanel                      │
│            │  [▶] [⏮] [⏭]  ══════════════════  [Side/Split] │
└────────────┴────────────────────────────────────────────────┘
```

---

## 2. 类设计

### 2.1 PlayerWindow

**继承**: `FluentWindow` (qfluentwidgets)

**公共 API**:
| 方法 | 参数 | 说明 |
|------|------|------|
| `load_sources` | `sources: list[str]` | 加载媒体源列表 |
| `set_sync_offset` | `index: int, offset_ms: int` | 设置时间偏移 |
| `set_view_mode` | `mode: str` | 切换视图模式 |
| `play()` | - | 开始播放 |
| `pause()` | - | 暂停播放 |
| `seek_to` | `timestamp_ms: int` | 跳转 |

**信号**:
| 信号 | 参数 | 触发时机 |
|------|------|----------|
| `source_loaded` | `(index, name)` | 源加载成功 |
| `source_error` | `(index, error)` | 源加载失败 |
| `play_state_changed` | `is_playing` | 播放状态变化 |
| `time_changed` | `time_ms` | 时间变化 |

### 2.2 GLWidget

**继承**: `QOpenGLWidget`

**职责**:
- 管理 OpenGL 上下文
- 编译和链接着色器程序
- 渲染双视频纹理合成

**关键方法**:
| 方法 | 说明 |
|------|------|
| `initializeGL()` | 初始化着色器、VBO、纹理 |
| `paintGL()` | 渲染帧 |
| `set_view_mode(mode)` | 切换视图模式 |
| `start_playback()` | 开始播放定时器 |
| `stop_playback()` | 停止播放定时器 |

### 2.3 ControlPanel

**组件**:
| 组件 | 类型 | 功能 |
|------|------|------|
| 播放按钮 | PushButton | 播放/暂停切换 |
| 逐帧按钮 | TransparentToolButton × 2 | 上/下一帧 |
| 时间轴 | Slider | 进度控制 |
| 时间显示 | BodyLabel | 当前/总时间 |
| 模式切换 | SegmentedWidget | Side-by-Side / Split |

**信号**:
- `play_clicked`
- `pause_clicked`
- `prev_frame_clicked`
- `next_frame_clicked`
- `seek_requested(int)`
- `view_mode_changed(ViewMode)`

---

## 3. 视图模式

### 3.1 ViewMode 枚举

| 值 | 模式 | 说明 |
|---|------|------|
| `SIDE_BY_SIDE = 0` | 并排 | 左半屏视频A，右半屏视频B |
| `SPLIT_SCREEN = 1` | 分屏 | 可拖动分割线位置 |

### 3.2 模式对比

```
Side-by-Side          Split-Screen
┌───────┬───────┐     ┌───────┬───────┐
│       │       │     │       │       │
│  A    │   B   │     │  A    │   B   │
│       │       │     │       │       │
└───────┴───────┘     └───┬───┴───────┘
  固定中线               可拖动分割线
```

---

## 4. Shader 设计

### 4.1 渲染管线

```
Vertex Shader          Fragment Shader
     │                      │
     ▼                      ▼
位置变换              纹理采样
UV 传递         ────▶ 视图模式判断
                       颜色输出
```

### 4.2 Uniform 变量

| 名称 | 类型 | 说明 |
|------|------|------|
| `texA` | sampler2D | 视频 A 纹理 |
| `texB` | sampler2D | 视频 B 纹理 |
| `split_position` | float | 分割线位置 (0.0~1.0) |
| `view_mode` | int | 0=并排, 1=分屏 |

### 4.3 片段着色器逻辑

```
if view_mode == SIDE_BY_SIDE:
    if uv.x < 0.5:
        color = texture(texA, uv_adjusted)
    else:
        color = texture(texB, uv_adjusted)
    draw_center_line()

else if view_mode == SPLIT_SCREEN:
    if uv.x < split_position:
        color = texture(texA, uv)
    else:
        color = texture(texB, uv)
    draw_split_line()
```

---

## 5. OpenGL Context 管理

### 5.1 问题

- FFmpeg 解码在后台线程
- OpenGL 渲染在主线程
- 纹理需要跨线程共享

### 5.2 解决方案

```
主线程                    后台线程
    │                         │
QOpenGLWidget              QOpenGLContext
    │                      (share context)
    │                         │
    └──── context 共享 ────────┘
```

**关键步骤**:
1. `GLWidget.initializeGL()` 中创建解码器
2. 调用 `decoder.set_opengl_context(self.context())`
3. C++ 层创建共享的 QOpenGLContext

---

## 6. 目录结构

```
player/
├── __init__.py           # 模块导出
├── player_window.py      # 主窗口
├── gl_widget.py          # OpenGL 画布
├── control_panel.py      # 控制面板
├── view_mode.py          # ViewMode 枚举
└── shaders/
    ├── vertex.glsl       # 顶点着色器
    └── fragment.glsl     # 片段着色器
```

---

## 7. 避免段错误原则

| 原则 | 说明 |
|------|------|
| FFmpeg 调用在 C++ | Python 不直接操作 AVFrame |
| 纹理由 C++ 管理 | Python 只持有 GLuint 整数 |
| 使用 RAII | C++ unique_ptr 管理资源 |
| 上下文检查 | 操作前验证 OpenGL 上下文有效 |

---

## 8. 验收标准

- [ ] PlayerWindow 正确显示并布局
- [ ] GLWidget 初始化 OpenGL 3.3 成功
- [ ] Shader 编译链接无错误
- [ ] 控制面板按钮响应正确
- [ ] 视图模式切换即时生效
- [ ] 时间轴拖动触发 seek

---

## 9. 依赖关系

**前置**:
- [native-core.md](native-core.md)
- [source-adapter.md](source-adapter.md)

**下一步**: [integration.md](integration.md)
