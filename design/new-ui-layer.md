# UI Layer - Python UI 层设计文档

> **版本**: 2.0
> **状态**: 设计阶段
> **依赖**: PySide6, qfluentwidgets, PyOpenGL

---

## 1. 模块概述

### 1.1 职责定义

| 组件 | 职责 |
|------|------|
| MainWindow | 主窗口、整体布局协调 |
| ToolBar | 顶部工具栏、视图模式切换、项目操作 |
| ViewportPanel | 视频预览区域、双视频并排渲染 |
| MediaInfoBar | 媒体名称显示、快速操作 |
| ControlsBar | 播放控制、缩放、速度调节 |
| TimelineArea | 时间轴轨道区域、多轨管理 |
| TrackRow | 单条轨道控制、可见性/静音/偏移 |
| GLWidget | OpenGL 渲染画布、着色器 |

### 1.2 窗口结构

```
┌─────────────────────────────────────────────────────────────────────┐
│                           MainWindow (QWidget)                       │
├─────────────────────────────────────────────────────────────────────┤
│  [并排] [分屏]              [添加媒体] [+新项目] [打开] [保存] [导出]  │  ToolBar
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│         ┌─────────────────────┬─────────────────────┐               │
│         │                     │                     │               │
│         │       视频 A        │       视频 B        │               │  ViewportPanel
│         │                     │                     │               │  (GLWidget × 2)
│         │                     │                     │               │
│         └─────────────────────┴─────────────────────┘               │
│                                                                      │
├──────────────────────────┬──────────────────────────────────────────┤
│  CityHall_1920x1080  ⚙ ✕ │  UshaikaRiverEmb_1920x1080  ⚙ ✕       │  MediaInfoBar
├──────────────────────────┴──────────────────────────────────────────┤
│  [↑] [🔍 121] [⏱ 1x] [⛶] [◁] [↻] [▷]  00:00/09.60  ═════●═══    │  ControlsBar
├──────────────────────────┬──────────────────────────────────────────┤
│ ✕ CityHall_1920x1080     │  👁 🔈 ◁ 00:00 ▷  ████████████████████  │  TrackRow 1
├──────────────────────────┼──────────────────────────────────────────┤
│ ✕ UshaikaRiverEmb_1920x1080 │  👁 🔈 ◁ 00:00 ▷  ████████████████████  │  TrackRow 2
└──────────────────────────┴──────────────────────────────────────────┘
```

---

## 2. 类设计

### 2.1 MainWindow

**继承**: `QWidget` (常规窗口，无导航栏)

**公共 API**:
| 方法 | 参数 | 说明 |
|------|------|------|
| `load_sources` | `sources: list[str]` | 加载媒体源列表 |
| `add_media` | `path: str` | 添加单个媒体 |
| `remove_media` | `index: int` | 移除媒体 |
| `set_sync_offset` | `index: int, offset_ms: int` | 设置时间偏移 |
| `set_view_mode` | `mode: ViewMode` | 切换视图模式 |
| `play()` | - | 开始播放 |
| `pause()` | - | 暂停播放 |
| `seek_to` | `timestamp_ms: int` | 跳转 |
| `new_project()` | - | 新建项目 |
| `open_project()` | `path: str` | 打开项目 |
| `save_project()` | `path: str` | 保存项目 |
| `export_report()` | `path: str` | 导出评测报告 |

**信号**:
| 信号 | 参数 | 触发时机 |
|------|------|----------|
| `source_loaded` | `(index, name)` | 源加载成功 |
| `source_error` | `(index, error)` | 源加载失败 |
| `play_state_changed` | `is_playing` | 播放状态变化 |
| `time_changed` | `time_ms` | 时间变化 |
| `track_added` | `index` | 轨道添加 |
| `track_removed` | `index` | 轨道移除 |

### 2.2 ToolBar

**继承**: `QWidget`

**组件**:
| 组件 | 类型 | 功能 |
|------|------|------|
| 视图模式组 | SegmentedWidget | 并排 / 分屏 切换 |
| 添加媒体按钮 | PrimaryPushButton | 添加媒体文件 |
| 新项目按钮 | PushButton | 新建项目 |
| 打开按钮 | TransparentToolButton | 打开项目 |
| 保存按钮 | TransparentToolButton | 保存项目 |
| 导出按钮 | PushButton | 导出报告 |
| 设置按钮 | TransparentToolButton | 打开设置 |
| 帮助按钮 | TransparentToolButton | 打开帮助 |

**信号**:
- `view_mode_changed(ViewMode)`
- `add_media_clicked()`
- `new_project_clicked()`
- `open_project_clicked()`
- `save_project_clicked()`
- `export_clicked()`
- `settings_clicked()`
- `help_clicked()`

### 2.3 ViewportPanel

**继承**: `QWidget`

**职责**:
- 管理两个 GLWidget 实例
- 响应视图模式切换
- 处理鼠标拖动分割线（分屏模式）

**布局**:
```
并排模式:
┌───────────────┬───────────────┐
│   GLWidget A  │   GLWidget B  │
└───────────────┴───────────────┘

分屏模式 (可拖动):
┌───────────┬───────────────────┐
│GLWidget A │    GLWidget B     │
└───────────┴───────────────────┘
        ↑ 分割线可拖动
```

**关键属性**:
| 属性 | 类型 | 说明 |
|------|------|------|
| `split_position` | float | 分割线位置 (0.1~0.9) |
| `view_mode` | ViewMode | 当前视图模式 |

### 2.4 MediaInfoBar

**继承**: `QWidget`

**组件** (每个媒体):
| 组件 | 类型 | 功能 |
|------|------|------|
| 媒体名称 | PushButton (带下拉) | 显示/选择媒体源 |
| 设置按钮 | TransparentToolButton | 媒体设置 |
| 关闭按钮 | TransparentToolButton | 移除该媒体 |

**信号**:
- `media_selected(index)`
- `media_settings_clicked(index)`
- `media_remove_clicked(index)`

### 2.5 ControlsBar

**继承**: `QWidget`

**组件**:
| 组件 | 类型 | 功能 |
|------|------|------|
| 展开按钮 | TransparentToolButton | 展开/折叠时间轴 |
| 缩放选择 | ComboBox | 缩放级别 (50%~400%) |
| 速度选择 | ComboBox | 播放速度 (0.25x~2x) |
| 全屏按钮 | TransparentToolButton | 全屏切换 |
| 上一帧 | TransparentToolButton | 逐帧后退 |
| 循环按钮 | TransparentToolButton | 循环/单次播放 |
| 播放按钮 | TransparentToolButton | 播放/暂停 |
| 时间显示 | BodyLabel | 当前时间 / 总时长 |
| 时间轴滑块 | Slider | 进度控制 |

**信号**:
- `play_clicked()`
- `pause_clicked()`
- `prev_frame_clicked()`
- `next_frame_clicked()`
- `loop_toggled(bool)`
- `seek_requested(int)`
- `zoom_changed(int)`
- `speed_changed(float)`
- `fullscreen_toggled()`

### 2.6 TimelineArea

**继承**: `QWidget`

**职责**:
- 管理多条 TrackRow
- 统一的时间标尺
- 播放头（时间指针）同步

**组件**:
| 组件 | 说明 |
|------|------|
| TrackRow 列表 | 每个媒体源对应一条轨道 |
| 时间标尺 | 顶部时间刻度显示 |
| 播放头 | 垂直绿色指示线 |

**公共 API**:
| 方法 | 说明 |
|------|------|
| `add_track(index, name)` | 添加轨道 |
| `remove_track(index)` | 移除轨道 |
| `update_playhead(position)` | 更新播放头位置 |

### 2.7 TrackRow

**继承**: `QWidget`

**左侧控制区 (320px)**:
| 组件 | 类型 | 功能 |
|------|------|------|
| 移除按钮 | TransparentToolButton | 移除该轨道 |
| 文件名 | BodyLabel | 媒体文件名 |
| 可见性按钮 | TransparentToolButton | 显示/隐藏视频 |
| 静音按钮 | TransparentToolButton | 静音/取消静音 |
| 偏移后退 | TransparentToolButton | 时间偏移 -1帧 |
| 偏移时间 | BodyLabel | 当前偏移值 |
| 偏移前进 | TransparentToolButton | 时间偏移 +1帧 |

**右侧轨道区**:
| 组件 | 说明 |
|------|------|
| 视频片段 | 灰色矩形表示视频时长 |
| 播放头线 | 垂直绿色指示线 |

**信号**:
- `remove_clicked()`
- `visibility_toggled(bool)`
- `mute_toggled(bool)`
- `offset_changed(int)` (毫秒)

### 2.8 GLWidget

**继承**: `QOpenGLWidget`

**职责**:
- 管理 OpenGL 上下文
- 编译和链接着色器程序
- 渲染视频纹理

**关键方法**:
| 方法 | 说明 |
|------|------|
| `initializeGL()` | 初始化着色器、VBO、纹理 |
| `paintGL()` | 渲染帧 |
| `set_texture(frame)` | 更新视频纹理 |
| `clear()` | 清空画面 |

---

## 3. 视图模式

### 3.1 ViewMode 枚举

| 值 | 模式 | 说明 |
|---|------|------|
| `SIDE_BY_SIDE = 0` | 并排 | 左右各占 50%，固定中线 |
| `SPLIT_SCREEN = 1` | 分屏 | 可拖动分割线位置 |

### 3.2 模式对比

```
Side-by-Side              Split-Screen
┌─────────┬─────────┐     ┌───────┬───────────┐
│         │         │     │       │           │
│    A    │    B    │     │   A   │     B     │
│         │         │     │       │           │
└─────────┴─────────┘     └───────┴───────────┘
  固定 50% 中线              可拖动分割线
```

---

## 4. Shader 设计

### 4.1 渲染管线

```
Vertex Shader              Fragment Shader
     │                          │
     ▼                          ▼
位置变换                  纹理采样
UV 传递           ────▶   颜色空间转换
                         颜色输出
```

### 4.2 Uniform 变量

| 名称 | 类型 | 说明 |
|------|------|------|
| `tex` | sampler2D | 视频纹理 (YUV 或 RGB) |
| `color_matrix` | mat3 | YUV->RGB 转换矩阵 |

### 4.3 着色器说明

每个 GLWidget 独立渲染单个视频，视图模式由 ViewportPanel 通过布局控制，无需在着色器中处理合成逻辑。

---

## 5. OpenGL Context 管理

### 5.1 问题

- FFmpeg 解码在后台线程
- OpenGL 渲染在主线程
- 纹理需要跨线程共享

### 5.2 解决方案

```
主线程                        后台线程
    │                             │
QOpenGLWidget                QOpenGLContext
    │                       (share context)
    │                             │
    └──── context 共享 ────────────┘
```

**关键步骤**:
1. `GLWidget.initializeGL()` 中创建解码器
2. 调用 `decoder.set_opengl_context(self.context())`
3. C++ 层创建共享的 QOpenGLContext

---

## 6. 目录结构

```
player/
├── __init__.py              # 模块导出
├── main_window.py           # 主窗口
├── toolbar.py               # 顶部工具栏
├── viewport_panel.py        # 视频预览区域
├── media_info_bar.py        # 媒体名称条
├── controls_bar.py          # 播放控制条
├── timeline_area.py         # 时间轴区域
├── track_row.py             # 单条轨道
├── gl_widget.py             # OpenGL 画布
├── view_mode.py             # ViewMode 枚举
└── shaders/
    ├── vertex.glsl          # 顶点着色器
    └── fragment.glsl        # 片段着色器
```

---

## 7. qfluentwidgets 控件映射

### 7.1 控件替换表

| 禁止使用 | 替代控件 |
|----------|----------|
| QMainWindow | QWidget |
| QDialog | MessageBoxBase |
| QPushButton | PushButton / PrimaryPushButton |
| QToolButton | TransparentToolButton |
| QLineEdit | LineEdit |
| QLabel | BodyLabel / CaptionLabel |
| QComboBox | ComboBox |
| QSlider | Slider (或 ProgressBar 作为显示) |
| QScrollArea | SmoothScrollArea |

### 7.2 布局规范

| 元素 | 间距 |
|------|------|
| 工具栏按钮间距 | 15px |
| 控制区组内间距 | 6px |
| 控制区间距 | 15px |
| 轨道控制区内边距 | 12px |
| 组件内元素间距 | 10px |

### 7.3 颜色规范

**原则**: 使用灰度色系，强调色取自 Windows 系统主题色

```python
# 灰度色系 (暗色主题)
GRAY_COLORS = {
    "bg_base": "#1e1e1e",        # 基础背景
    "bg_elevated": "#2d2d2d",    # 抬起层背景
    "bg_overlay": "#333333",     # 覆盖层背景
    "text_primary": "#e0e0e0",   # 主要文字
    "text_secondary": "#888888", # 次要文字
    "border": "#3e3e3e",         # 边框
    "clip": "#404455",           # 时间轴片段 (灰蓝调)
}

# 强调色 - 从 Windows 系统获取
def get_accent_color() -> QColor:
    """获取 Windows 系统主题色"""
    from PySide6.QtGui import QGuiApplication

    palette = QGuiApplication.palette()
    # QPalette.Highlight 是系统强调色
    return palette.color(QPalette.ColorRole.Highlight)
```

**使用示例**:
```python
from PySide6.QtGui import QPalette
from qfluentwidgets import isDarkTheme, themeColor

# 方式1: 使用 qfluentwidgets 主题色 (自动跟随系统)
accent = themeColor()

# 方式2: 直接获取系统色
palette = self.palette()
accent = palette.color(QPalette.ColorRole.Highlight)

# 应用到控件
label.setStyleSheet(f"color: {accent.name()};")
```

**灰度层级**:
| 用途 | 明度 | 示例 |
|------|------|------|
| 最深背景 | 10% | #1a1a1a |
| 基础背景 | 12% | #1e1e1e |
| 控制区背景 | 15% | #252525 |
| 抬起层背景 | 18% | #2d2d2d |
| 时间轴背景 | 17% | #2b2b2b |
| 轨道控制区 | 20% | #333333 |
| 边框 | 25% | #3e3e3e |

---

## 8. 避免段错误原则

| 原则 | 说明 |
|------|------|
| FFmpeg 调用在 C++ | Python 不直接操作 AVFrame |
| 纹理由 C++ 管理 | Python 只持有 GLuint 整数 |
| 使用 RAII | C++ unique_ptr 管理资源 |
| 上下文检查 | 操作前验证 OpenGL 上下文有效 |

---

## 9. 验收标准

- [ ] MainWindow 正确显示整体布局
- [ ] ToolBar 按钮响应正确，视图模式切换生效
- [ ] ViewportPanel 双视频并排显示
- [ ] GLWidget 初始化 OpenGL 3.3 成功
- [ ] Shader 编译链接无错误
- [ ] MediaInfoBar 显示媒体名称
- [ ] ControlsBar 播放控制响应正确
- [ ] TimelineArea 显示多轨道
- [ ] TrackRow 可见性/静音/偏移控制生效
- [ ] 时间轴拖动触发 seek
- [ ] 分屏模式分割线可拖动

---

## 10. 依赖关系

**前置**:
- [native-core.md](native-core.md)
- [source-adapter.md](source-adapter.md)

**下一步**: [integration.md](integration.md)
