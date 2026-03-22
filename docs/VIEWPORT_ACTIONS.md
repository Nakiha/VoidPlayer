# Viewport Actions 设计文档

本文档描述 viewport 的缩放和画面移动功能的实现设计。

## 1. 自定义 ComboBox 控件

### 1.1 控件结构

```
┌─────────────────────────────────────────────┐
│ [图标/文字] [────EditBox────] [▼ 下拉按钮] │
└─────────────────────────────────────────────┘
```

**三个子组件**：

| 组件 | 说明 |
|------|------|
| **Leading 元素** | 图标或文字，用于提示功能（如放大镜图标、倍速文字） |
| **EditBox** | 显示当前值，允许用户任意编辑输入 |
| **下拉按钮** | 点击展开预设列表，选择后计算值填入 EditBox |

**样式规范**：
- 各子组件无 border 和 padding
- 整个控件外绘制一圈圆角 border（Fluent Design 风格）
- 复用 PySide6-Fluent-Widgets 的组件元素保持风格一致

### 1.2 类设计

```python
# player/ui/widgets/custom_combo_box.py

class CustomComboBox(QWidget):
    """自定义组合下拉框控件

    结构: [leading][editbox][dropdown_button]
    """

    # 信号
    value_changed = Signal(str)  # 值变化时发出（用户编辑或选择预设）

    def __init__(self, parent=None):
        super().__init__(parent)
        self._leading_widget = None  # QWidget 或 None
        self._edit_line = None       # QLineEdit
        self._dropdown_btn = None    # QPushButton
        self._popup_list = None      # 下拉列表 popup
        self._presets = []           # 预设值列表 [(display_text, value), ...]
        self._validator = None       # 输入校验器（可选）

    # --- 子组件设置 ---
    def set_leading_icon(self, icon: QIcon):
        """设置 leading 为图标"""

    def set_leading_text(self, text: str):
        """设置 leading 为文字"""

    def set_presets(self, presets: list[tuple[str, str]]):
        """设置预设列表 [(display, value), ...]"""

    def set_validator(self, validator: QValidator):
        """设置 editbox 输入校验器"""

    # --- 值操作 ---
    def value(self) -> str:
        """获取当前 editbox 值"""

    def set_value(self, value: str, emit: bool = True):
        """设置 editbox 值"""

    # --- Hook 接口 ---
    def on_preset_selected(self, preset_value: str) -> str:
        """Hook: 预设被选中时调用，返回实际要填入 editbox 的值

        子类可重写此方法实现自定义逻辑，如 fit 值的计算
        """
        return preset_value

    def on_value_editing_finished(self, value: str) -> str:
        """Hook: editbox 编辑完成时调用，返回处理后的值

        子类可重写此方法实现值的钳制或转换
        """
        return value
```

### 1.3 缩放 ComboBox

```python
# player/ui/widgets/zoom_combo_box.py

class ZoomComboBox(CustomComboBox):
    """缩放控制 ComboBox

    显示: [放大镜图标][100%][下拉按钮]
    预设: fit, 100%, 200%, 300%, ..., 1000%
    """

    # 信号
    zoom_changed = Signal(float)  # 缩放比例变化，1.0 = 100%

    def __init__(self, parent=None):
        super().__init__(parent)
        self._fit_value = 1.0  # 当前 fit 计算值

        self.set_leading_icon(FluentIcon.ZOOM.icon())
        self.set_presets([
            ("Fit", "fit"),
            ("100%", "1.0"),
            ("200%", "2.0"),
            ("300%", "3.0"),
            ("400%", "4.0"),
            ("500%", "5.0"),
            ("600%", "6.0"),
            ("700%", "7.0"),
            ("800%", "8.0"),
            ("900%", "9.0"),
            ("1000%", "10.0"),
        ])

    def set_fit_value(self, fit_value: float):
        """设置当前 fit 计算值（动态更新）"""
        self._fit_value = fit_value

    def on_preset_selected(self, preset_value: str) -> str:
        """预设选择处理"""
        if preset_value == "fit":
            # 返回 fit 的实际计算值
            return f"{int(self._fit_value * 100)}%"
        # 其他预设，检查是否小于 fit
        ratio = float(preset_value)
        if ratio < self._fit_value:
            return f"{int(self._fit_value * 100)}%"
        return f"{int(ratio * 100)}%"

    def on_value_editing_finished(self, value: str) -> str:
        """编辑完成处理，钳制到有效范围"""
        try:
            # 解析用户输入（支持 "150%" 或 "1.5" 格式）
            ratio = self._parse_zoom_value(value)
            # 最小值为 fit，最大值无限制
            ratio = max(ratio, self._fit_value)
            return f"{int(ratio * 100)}%"
        except ValueError:
            return f"{int(self._fit_value * 100)}%"

    def _parse_zoom_value(self, value: str) -> float:
        """解析缩放值字符串为比例"""
        value = value.strip().rstrip('%')
        return float(value) / 100.0

    def get_zoom_ratio(self) -> float:
        """获取当前缩放比例"""
        return self._parse_zoom_value(self.value())
```

### 1.4 倍速 ComboBox（预留）

```python
# player/ui/widgets/speed_combo_box.py

class SpeedComboBox(CustomComboBox):
    """倍速控制 ComboBox

    显示: [1.0x][下拉按钮]  (无 leading 图标，或用文字 "Speed")
    预设: 0.5x, 1.0x, 1.5x, 2.0x, 4.0x
    范围: 0.5x ~ 4.0x
    """

    speed_changed = Signal(float)  # 倍速变化

    def __init__(self, parent=None):
        super().__init__(parent)
        self.set_presets([
            ("0.5x", "0.5"),
            ("1.0x", "1.0"),
            ("1.5x", "1.5"),
            ("2.0x", "2.0"),
            ("4.0x", "4.0"),
        ])

    def on_value_editing_finished(self, value: str) -> str:
        """钳制到 0.5 ~ 4.0 范围"""
        # ... 实现略
```

---

## 2. Viewport 缩放 Action

### 2.1 概念定义

| 术语 | 定义 |
|------|------|
| **片分辨率** | 视频轨道的原始分辨率 (width × height) |
| **分割视图** | 多轨道时，每个 track 在 QOpenGLWidget 中占用的区域 |
| **fit 值** | 锁定画面宽高比填充分割视图最短边所需的缩放比例 |
| **缩放比例** | 1.0 = 原始大小，2.0 = 放大 2 倍 |
| **像素当量** | 视频一个像素在分割视图中占据的实际像素数 |

### 2.2 Fit 值计算

```python
def calculate_fit_ratio(track_size: QSizeF, viewport_size: QSizeF) -> float:
    """计算 fit 缩放比例

    Args:
        track_size: track 的片分辨率
        viewport_size: 分割视图的大小

    Returns:
        使画面填满最短边所需的缩放比例

    Example:
        track = 1920x1080, viewport = 960x1000
        fit_w = 960 / 1920 = 0.5   # 按宽度缩放
        fit_h = 1000 / 1080 = 0.926  # 按高度缩放
        取较大值以填满最短边: max(0.5, 0.926) = 0.926
    """
    fit_w = viewport_size.width() / track_size.width()
    fit_h = viewport_size.height() / track_size.height()
    return max(fit_w, fit_h)
```

### 2.3 单轨道缩放逻辑

**滚轮缩放**：
- 上滚：缩放比例 /= 0.94（放大）
- 下滚：缩放比例 *= 0.94（缩小）
- 最小值：当前 fit 计算值
- 最大值：无限制

**缩放中心计算**：

```
场景A：画面不能填满分割视图（有黑边）
    以分割视图中心为缩放中心

场景B：画面能填满分割视图（无黑边）
    以鼠标在分割视图中的归一化位置为缩放中心
```

**场景B 的缩放偏移计算**：

```python
def calculate_zoom_offset(
    old_view_rect: QRectF,      # 缩放前的视图截取区域（片坐标系）
    zoom_factor: float,         # 缩放因子（0.94 或 1/0.94）
    mouse_norm: QPointF,        # 鼠标归一化位置 (0~1, 0~1)
) -> QRectF:
    """计算缩放后的新视图区域

    原理：
    1. 计算缩放后区域大小
    2. 根据鼠标位置计算偏移方向
    3. 计算新的左上角位置
    """
    # 缩放后区域大小
    new_width = old_view_rect.width() * zoom_factor
    new_height = old_view_rect.height() * zoom_factor

    # 缩放前后尺寸差
    dw = old_view_rect.width() - new_width
    dh = old_view_rect.height() - new_height

    # 新的左上角位置（向鼠标方向偏移）
    new_x = old_view_rect.x() + dw * mouse_norm.x()
    new_y = old_view_rect.y() + dh * mouse_norm.y()

    return QRectF(new_x, new_y, new_width, new_height)
```

**示例计算**（来自需求描述）：

```
缩放前：
  - 控件视图可渲染面积：3840×2160
  - viewport 大小：3840×1700
  - 左上角位置：(0, 230)  # 图像坐标系

缩放操作：上滚，zoom_factor = 0.94

缩放后：
  - 新区域大小：3840×0.94 × 1700×0.94 = 3609.6×1598
  - 尺寸差：dw=230.4, dh=102

鼠标位置：(1.0, 1.0)  # 右下角，归一化

新左上角：
  x = 0 + 230.4 × 1.0 = 230.4
  y = 230 + 102 × 1.0 = 332

结果：从 (230.4, 332) 裁剪 3609.6×1598 的区域
```

### 2.4 Action 定义

```python
# player/core/actions/viewport_zoom.py

class ViewportZoomAction:
    """Viewport 缩放 Action

    触发方式：
    1. 鼠标滚轮（在 viewport 上）
    2. ControlsBar 的 ZoomComboBox 编辑/选择
    """

    ACTION_ID = "viewport_zoom"

    def __init__(self, viewport_manager):
        self._viewport_manager = viewport_manager
        self._current_zoom = 1.0  # 当前缩放比例

    # --- 滚轮触发 ---
    def on_wheel(self, delta: int, mouse_pos: QPointF, track_index: int):
        """处理滚轮事件

        Args:
            delta: 滚轮增量（正=上滚，负=下滚）
            mouse_pos: 鼠标在 QOpenGLWidget 中的位置
            track_index: 鼠标所在的 track 索引
        """
        # 计算新缩放比例
        if delta > 0:
            new_zoom = self._current_zoom / 0.94
        else:
            new_zoom = self._current_zoom * 0.94

        # 钳制到最小值（fit）
        min_zoom = self._viewport_manager.get_min_zoom()
        new_zoom = max(new_zoom, min_zoom)

        # 执行缩放
        self._apply_zoom(new_zoom, mouse_pos, track_index)

    # --- ComboBox 触发 ---
    def on_zoom_value_changed(self, zoom_ratio: float):
        """处理 ComboBox 值变化"""
        self._apply_zoom(zoom_ratio, None, None)

    def _apply_zoom(self, zoom_ratio: float, mouse_pos: QPointF, track_index: int):
        """应用缩放"""
        # ... 实现详见多轨道部分
```

---

## 3. Viewport 画面移动 Action

### 3.1 移动逻辑

**场景区分**：

| 场景 | 条件 | 移动限制 |
|------|------|----------|
| 有黑边 | 最大分辨率 track 的画面不能填满分割视图 | 只能沿有黑边的轴移动 |
| 无黑边 | 最大分辨率 track 的画面填满分割视图 | 不能移出导致出现黑边 |

**边界钳制**（无黑边时）：

```python
def clamp_view_offset(
    view_rect: QRectF,      # 当前视图截取区域
    track_size: QSizeF,     # 片分辨率
) -> QPointF:
    """钳制视图偏移，确保不出现黑边

    Returns:
        有效的左上角位置
    """
    # 允许的最大左上角位置（确保右下角不超出片边界）
    max_x = track_size.width() - view_rect.width()
    max_y = track_size.height() - view_rect.height()

    # 如果视图比片大（有黑边情况），允许的偏移范围
    if max_x < 0:
        max_x = 0
        # 水平方向有黑边，限制水平移动范围
    if max_y < 0:
        max_y = 0
        # 垂直方向有黑边，限制垂直移动范围

    # 钳制
    clamped_x = max(0, min(view_rect.x(), max_x))
    clamped_y = max(0, min(view_rect.y(), max_y))

    return QPointF(clamped_x, clamped_y)
```

### 3.2 Action 定义

```python
# player/core/actions/viewport_pan.py

class ViewportPanAction:
    """Viewport 画面移动 Action

    触发方式：鼠标拖拽（在 viewport 上）
    """

    ACTION_ID = "viewport_pan"

    def __init__(self, viewport_manager):
        self._viewport_manager = viewport_manager
        self._is_panning = False
        self._last_pos = None

    def on_mouse_press(self, pos: QPointF, track_index: int):
        """开始拖拽"""
        self._is_panning = True
        self._last_pos = pos

    def on_mouse_move(self, pos: QPointF, track_index: int):
        """拖拽移动"""
        if not self._is_panning:
            return

        delta = pos - self._last_pos
        self._last_pos = pos

        self._apply_pan(delta, track_index)

    def on_mouse_release(self):
        """结束拖拽"""
        self._is_panning = False
        self._last_pos = None

    def _apply_pan(self, delta: QPointF, track_index: int):
        """应用移动"""
        # ... 实现详见多轨道部分
```

---

## 4. 多轨道特殊处理

### 4.1 分割视图计算

```python
# player/core/viewport/track_layout.py

class TrackLayout:
    """多轨道布局管理

    计算每个 track 的分割视图区域
    """

    def __init__(self):
        self._tracks = []  # TrackInfo 列表
        self._split_ratio = 1/3  # 分割线位置（对于 2 轨道）

    def update_layout(self, widget_size: QSize, track_count: int):
        """更新布局

        Args:
            widget_size: QOpenGLWidget 的总大小
            track_count: track 数量
        """
        # 计算每个 track 的分割视图区域
        if track_count == 1:
            self._tracks = [TrackRegion(0, QRectF(0, 0, widget_size.width(), widget_size.height()))]
        elif track_count == 2:
            split_x = widget_size.width() * self._split_ratio
            self._tracks = [
                TrackRegion(0, QRectF(0, 0, split_x, widget_size.height())),
                TrackRegion(1, QRectF(split_x, 0, widget_size.width() - split_x, widget_size.height())),
            ]
        # ... 更多轨道数支持

    def get_track_at(self, pos: QPointF) -> int:
        """获取指定位置所属的 track 索引

        Example:
            widget = 1920×1000, 2 tracks, 1/3 分割
            pos = (640, 500) → track 1 (属于右侧区域)
            pos = (320, 500) → track 0 (属于左侧区域)
        """
        for track in self._tracks:
            if track.region.contains(pos):
                return track.index
        return -1

    def get_viewport_region(self, track_index: int) -> QRectF:
        """获取指定 track 的分割视图区域"""
        return self._tracks[track_index].region

@dataclass
class TrackRegion:
    index: int
    region: QRectF  # 分割视图区域（QOpenGLWidget 坐标系）
```

### 4.2 鼠标归一化位置计算

```python
def normalize_mouse_position(
    widget_pos: QPointF,   # 鼠标在 QOpenGLWidget 中的位置
    track_region: QRectF,  # 所属 track 的分割视图区域
) -> QPointF:
    """计算鼠标在分割视图中的归一化位置

    Args:
        widget_pos: QOpenGLWidget 坐标系中的鼠标位置
        track_region: track 的分割视图区域

    Returns:
        归一化位置 (0~1, 0~1)

    Example:
        widget = 1920×1000, 2 tracks, 1/3 分割
        track_1 region = (640, 0, 1280, 1000)
        mouse at (640, 500) in widget
        → normalized = ((640-640)/1280, (500-0)/1000) = (0.0, 0.5)
    """
    local_x = widget_pos.x() - track_region.left()
    local_y = widget_pos.y() - track_region.top()

    norm_x = local_x / track_region.width()
    norm_y = local_y / track_region.height()

    return QPointF(norm_x, norm_y)
```

### 4.3 像素当量统一

**核心原则**：所有 track 的像素当量必须一致

```python
def calculate_pixel_equivalent(
    zoom_ratio: float,     # 缩放比例（基于最大分辨率 track）
    max_track_size: QSizeF # 最大分辨率 track 的片分辨率
) -> float:
    """计算像素当量

    像素当量 = 视频一个像素在分割视图中占据的实际像素数

    Example:
        max_track = 1920×1080
        viewport = 960×1000
        zoom = 1.0

        fit = max(960/1920, 1000/1080) = 0.926
        在 zoom=1.0 时，像素当量 = 1.0

        实际显示：
        track_1 (1080p): 1920 × 1.0 = 1920 像素显示在 960×1000 的区域
        track_0 (720p): 1280 × 1.0 = 1280 像素显示在同一像素当量下
    """
    return zoom_ratio
```

### 4.4 多轨道缩放实现

```python
class ViewportManager:
    """Viewport 状态管理"""

    def __init__(self):
        self._zoom_ratio = 1.0
        self._view_offset = QPointF(0, 0)  # 视图偏移（片坐标系）
        self._track_layout = TrackLayout()
        self._tracks = []  # Track 视频信息

    def get_max_track(self) -> TrackInfo:
        """获取最大分辨率的 track"""
        return max(self._tracks, key=lambda t: t.width * t.height)

    def get_min_zoom(self) -> float:
        """获取最小缩放比例（最大分辨率 track 的 fit 值）"""
        max_track = self.get_max_track()
        viewport = self._track_layout.get_viewport_region(max_track.index)
        return calculate_fit_ratio(max_track.size, viewport.size())

    def apply_zoom(
        self,
        zoom_ratio: float,
        mouse_widget_pos: QPointF,  # QOpenGLWidget 坐标
        track_index: int,
    ):
        """应用缩放（所有 track 同步）"""
        max_track = self.get_max_track()

        # 钳制缩放比例
        min_zoom = self.get_min_zoom()
        zoom_ratio = max(zoom_ratio, min_zoom)

        old_zoom = self._zoom_ratio
        self._zoom_ratio = zoom_ratio

        # 计算缩放因子
        zoom_factor = old_zoom / zoom_ratio if zoom_ratio > 0 else 1.0

        # 计算鼠标归一化位置
        if mouse_widget_pos and track_index >= 0:
            track_region = self._track_layout.get_viewport_region(track_index)
            mouse_norm = normalize_mouse_position(mouse_widget_pos, track_region)
        else:
            # 无鼠标位置时使用中心
            mouse_norm = QPointF(0.5, 0.5)

        # 判断是否有黑边
        max_viewport = self._track_layout.get_viewport_region(max_track.index)
        has_black_bars = self._check_black_bars(max_track, max_viewport, zoom_ratio)

        if has_black_bars:
            # 有黑边时以中心缩放
            self._view_offset = QPointF(0, 0)
        else:
            # 无黑边时以鼠标位置缩放
            self._view_offset = self._calculate_zoom_offset(
                zoom_factor, mouse_norm, max_track
            )

        # 更新 UI
        self._update_controls_bar()

    def _check_black_bars(
        self,
        track: TrackInfo,
        viewport: QRectF,
        zoom_ratio: float,
    ) -> bool:
        """检查是否有黑边"""
        display_w = track.width * zoom_ratio
        display_h = track.height * zoom_ratio
        return display_w < viewport.width() or display_h < viewport.height()

    def _calculate_zoom_offset(
        self,
        zoom_factor: float,
        mouse_norm: QPointF,
        max_track: TrackInfo,
    ) -> QPointF:
        """计算缩放后的视图偏移"""
        # 视图在片坐标系中的大小
        max_viewport = self._track_layout.get_viewport_region(max_track.index)
        view_w = max_viewport.width() / self._zoom_ratio
        view_h = max_viewport.height() / self._zoom_ratio

        # 缩放后大小
        new_view_w = view_w * zoom_factor
        new_view_h = view_h * zoom_factor

        # 尺寸差
        dw = view_w - new_view_w
        dh = view_h - new_view_h

        # 新偏移
        new_x = self._view_offset.x() + dw * mouse_norm.x()
        new_y = self._view_offset.y() + dh * mouse_norm.y()

        return QPointF(new_x, new_y)
```

### 4.5 多轨道移动实现

```python
def apply_pan(self, delta: QPointF, track_index: int):
    """应用画面移动（所有 track 同步）"""
    max_track = self.get_max_track()
    max_viewport = self._track_layout.get_viewport_region(max_track.index)

    # 将屏幕像素移动转换为片坐标移动
    # delta 是屏幕坐标，需要转换为片坐标系
    scale = 1.0 / self._zoom_ratio
    pan_x = delta.x() * scale
    pan_y = delta.y() * scale

    # 计算新偏移
    new_offset = QPointF(
        self._view_offset.x() + pan_x,
        self._view_offset.y() + pan_y,
    )

    # 边界钳制（只针对最大分辨率 track）
    new_offset = self._clamp_offset(new_offset, max_track, max_viewport)

    self._view_offset = new_offset

def _clamp_offset(
    self,
    offset: QPointF,
    track: TrackInfo,
    viewport: QRectF,
) -> QPointF:
    """钳制偏移，确保不出现黑边（针对最大分辨率 track）"""
    # 视图在片坐标系中的大小
    view_w = viewport.width() / self._zoom_ratio
    view_h = viewport.height() / self._zoom_ratio

    # 检查是否有黑边
    if view_w > track.width:
        # 水平方向有黑边，限制水平移动
        max_x = 0
    else:
        max_x = track.width - view_w

    if view_h > track.height:
        # 垂直方向有黑边，限制垂直移动
        max_y = 0
    else:
        max_y = track.height - view_h

    return QPointF(
        max(0, min(offset.x(), max_x)),
        max(0, min(offset.y(), max_y)),
    )
```

---

## 5. 窗体缩放处理

当 QOpenGLWidget 大小变化时：

```python
def on_widget_resize(self, new_size: QSize):
    """处理控件大小变化"""
    # 1. 更新布局
    self._track_layout.update_layout(new_size, len(self._tracks))

    # 2. 重新计算 fit 值
    new_fit = self.get_min_zoom()

    # 3. 如果当前缩放小于新 fit 值，钳制到 fit
    if self._zoom_ratio < new_fit:
        self._zoom_ratio = new_fit
        self._update_controls_bar()

    # 4. 重新钳制偏移
    max_track = self.get_max_track()
    max_viewport = self._track_layout.get_viewport_region(max_track.index)
    self._view_offset = self._clamp_offset(self._view_offset, max_track, max_viewport)

    # 5. 触发重绘
    self._request_redraw()
```

---

## 6. 缩放与移动的交互

**规则**：
1. 缩放操作会改变 `_view_offset`（以鼠标位置为缩放中心时）
2. 移动操作会改变 `_view_offset`
3. 任何偏移变化都需要钳制到有效范围
4. 缩放后自动检查是否出现黑边，调整偏移

```python
def _ensure_valid_offset(self):
    """确保偏移值有效"""
    max_track = self.get_max_track()
    max_viewport = self._track_layout.get_viewport_region(max_track.index)
    self._view_offset = self._clamp_offset(self._view_offset, max_track, max_viewport)
```

---

## 7. Action 注册

```python
# player/core/actions/registry.py

ACTIONS = {
    # ... 其他 actions
    "viewport_zoom": {
        "class": "player.core.actions.viewport_zoom.ViewportZoomAction",
        "description": "Viewport zoom control",
    },
    "viewport_pan": {
        "class": "player.core.actions.viewport_pan.ViewportPanAction",
        "description": "Viewport pan control",
    },
}
```

---

## 8. 快捷键绑定

```python
# player/core/shortcuts.py

# 滚轮缩放：鼠标滚轮（无需快捷键，直接在 viewport 事件中处理）
# 画面移动：鼠标拖拽（无需快捷键，直接在 viewport 事件中处理）

# 可选：快捷键重置视图
SHORTCUTS = {
    # ... 其他快捷键
    "reset_view": {
        "key": "Ctrl+0",
        "action": "viewport_reset",
        "description": "Reset viewport to fit",
    },
}
```

---

## 9. 文件结构

```
player/
├── ui/
│   └── widgets/
│       ├── custom_combo_box.py      # 自定义 ComboBox 基类
│       ├── zoom_combo_box.py        # 缩放 ComboBox
│       └── speed_combo_box.py       # 倍速 ComboBox（预留）
├── core/
│   └── actions/
│       ├── viewport_zoom.py         # 缩放 Action
│       └── viewport_pan.py          # 移动 Action
│   └── viewport/
│       ├── manager.py               # ViewportManager
│       └── track_layout.py          # 多轨道布局
└── ...
```

---

## 10. 布局模式

### 10.1 并排模式（Side by Side）

- 每个 track 的分割视图固定为 **1/n** 等分
- 最多支持 **8 个轨道**
- 所有分割视图宽度相同

```
┌─────┬─────┬─────┬─────┐
│  1  │  2  │  3  │  4  │  (4 tracks, 各占 1/4)
└─────┴─────┴─────┴─────┘
```

### 10.2 分屏模式（Split）

- 只有 **2 个视图**
- **分割比例可调整**（用户可拖动分割线）
- 默认 1/3 : 2/3 分割

```
┌───────┬───────────────┐
│   1   │       2       │  (可调整分割线位置)
└───────┴───────────────┘
```

### 10.3 布局模式切换

当布局模式或分割比例变化时：
1. 重新计算各 track 的分割视图区域
2. 重新计算 fit 值
3. 必要时钳制缩放和偏移

---

## 11. Track 动态变化处理

### 11.1 运行时状态

- 缩放比例 `_zoom_ratio` 和偏移 `_view_offset` **仅在本次运行中保持**
- 不持久化到配置或项目文件

### 11.2 Add Track 处理

当添加新 track 时：

```python
def on_track_added(self, new_track: TrackInfo):
    """处理新 track 添加"""
    # 1. 更新 track 列表
    self._tracks.append(new_track)

    # 2. 更新布局
    self._track_layout.update_layout(
        self._widget_size,
        len(self._tracks)
    )

    # 3. 检查最大分辨率 track 是否变化
    old_max = self._max_track
    new_max = self.get_max_track()

    if new_max == new_track and new_track != old_max:
        # 新 track 成为最大分辨率 track
        self._on_max_track_changed(old_max, new_max)
    else:
        # 新 track 非最大分辨率，只需重新计算 fit 并钳制
        new_fit = self.get_min_zoom()
        if self._zoom_ratio < new_fit:
            self._zoom_ratio = new_fit
        self._ensure_valid_offset()

    self._request_redraw()
```

### 11.3 Remove Track 处理

当移除 track 时：

```python
def on_track_removed(self, removed_track: TrackInfo):
    """处理 track 移除"""
    # 1. 更新 track 列表
    self._tracks.remove(removed_track)

    # 2. 更新布局
    self._track_layout.update_layout(
        self._widget_size,
        len(self._tracks)
    )

    # 3. 检查被移除的是否是最大分辨率 track
    if len(self._tracks) == 0:
        # 无 track，重置状态
        self._zoom_ratio = 1.0
        self._view_offset = QPointF(0, 0)
    else:
        new_max = self.get_max_track()
        if removed_track == self._last_max_track:
            # 最大分辨率 track 被移除
            self._on_max_track_changed(removed_track, new_max)

    self._request_redraw()
```

### 11.4 最大分辨率 Track 变化处理

当最大分辨率 track 变化时（关键逻辑）：

```python
def _on_max_track_changed(self, old_max: TrackInfo, new_max: TrackInfo):
    """处理最大分辨率 track 变化

    这是状态转换的关键点：
    1. 缩放比例需要重新解释（相对于新 max 的 fit）
    2. 偏移需要转换坐标系并钳制
    """
    if old_max is None:
        # 首次添加 track
        self._zoom_ratio = self.get_min_zoom()  # 初始化为 fit
        self._view_offset = QPointF(0, 0)
        return

    # 计算旧的 fit 值
    old_viewport = self._track_layout.get_viewport_region_by_size(
        old_max.size, len(self._tracks) + 1  # 布局可能已变化
    )
    old_fit = calculate_fit_ratio(old_max.size, old_viewport.size())

    # 计算新的 fit 值
    new_fit = self.get_min_zoom()

    # 转换缩放比例
    # zoom_ratio 是相对于片分辨率的，需要保持视觉效果一致
    # 旧: old_display_size = old_max.size * zoom_ratio
    # 新: new_display_size = new_max.size * new_zoom_ratio
    # 保持像素当量一致: zoom_ratio / old_fit = new_zoom_ratio / new_fit
    #
    # 简化：保持当前显示效果，直接用 new_fit 作为新的最小基准
    # 如果当前 zoom < new_fit，钳制到 new_fit
    if self._zoom_ratio < new_fit:
        self._zoom_ratio = new_fit
        self._view_offset = QPointF(0, 0)  # 重置偏移
    else:
        # 缩放比例有效，转换偏移
        self._view_offset = self._convert_offset(old_max, new_max)
        self._ensure_valid_offset()

    self._update_controls_bar()

def _convert_offset(self, old_max: TrackInfo, new_max: TrackInfo) -> QPointF:
    """转换偏移到新坐标系

    偏移是片坐标系中的位置，需要根据新片分辨率调整
    如果新片更大，偏移可能需要按比例缩放
    如果新片更小，偏移会被钳制
    """
    scale_x = new_max.width / old_max.width
    scale_y = new_max.height / old_max.height

    new_offset = QPointF(
        self._view_offset.x() * scale_x,
        self._view_offset.y() * scale_y,
    )

    return new_offset
```

---

## 12. 交互响应要求

### 12.1 移动交互

- **无平滑动画**：直接应用偏移变化
- **追随鼠标**：必须实时响应鼠标拖动
- **适配刷新率**：每次屏幕刷新（vsync）时更新位置

```python
def on_mouse_move(self, pos: QPointF, track_index: int):
    """鼠标移动处理 - 每帧调用"""
    if not self._is_panning:
        return

    delta = pos - self._last_pos
    self._last_pos = pos

    # 直接应用，无动画
    self._apply_pan(delta, track_index)

    # 请求立即重绘（vsync 同步）
    self._request_redraw()
```

### 12.2 缩放交互

- **无平滑动画**：滚轮事件直接触发缩放
- **ComboBox 变化**：直接应用新值

```python
def on_wheel(self, delta: int, mouse_pos: QPointF, track_index: int):
    """滚轮缩放 - 直接应用"""
    if delta > 0:
        new_zoom = self._zoom_ratio / 0.94
    else:
        new_zoom = self._zoom_ratio * 0.94

    # 钳制并直接应用
    new_zoom = max(new_zoom, self.get_min_zoom())
    self._apply_zoom(new_zoom, mouse_pos, track_index)
```

---

## 13. 布局管理器更新

```python
# player/core/viewport/track_layout.py

class LayoutMode(Enum):
    SIDE_BY_SIDE = "side_by_side"  # 并排模式：1/n 等分
    SPLIT = "split"                 # 分屏模式：可调分割比

class TrackLayout:
    """多轨道布局管理"""

    def __init__(self):
        self._mode = LayoutMode.SIDE_BY_SIDE
        self._split_ratio = 1/3  # 分屏模式下的分割比例
        self._tracks = []

    def set_mode(self, mode: LayoutMode):
        """设置布局模式"""
        self._mode = mode
        self._invalidate_layout()

    def set_split_ratio(self, ratio: float):
        """设置分屏模式的分割比例 (0.1 ~ 0.9)"""
        self._split_ratio = max(0.1, min(0.9, ratio))
        self._invalidate_layout()

    def update_layout(self, widget_size: QSize, track_count: int):
        """更新布局"""
        self._tracks.clear()

        if track_count == 0:
            return

        if self._mode == LayoutMode.SIDE_BY_SIDE:
            # 并排模式：1/n 等分
            segment_width = widget_size.width() / track_count
            for i in range(track_count):
                region = QRectF(
                    i * segment_width, 0,
                    segment_width, widget_size.height()
                )
                self._tracks.append(TrackRegion(i, region))

        elif self._mode == LayoutMode.SPLIT:
            # 分屏模式：最多 2 个视图
            if track_count == 1:
                region = QRectF(0, 0, widget_size.width(), widget_size.height())
                self._tracks.append(TrackRegion(0, region))
            else:
                split_x = widget_size.width() * self._split_ratio
                self._tracks.append(TrackRegion(0, QRectF(
                    0, 0, split_x, widget_size.height()
                )))
                self._tracks.append(TrackRegion(1, QRectF(
                    split_x, 0, widget_size.width() - split_x, widget_size.height()
                )))
                # 如果 track_count > 2，忽略多余的（或报错）
```

---

## 14. 完整状态转换图

```
┌─────────────────────────────────────────────────────────────┐
│                     ViewportManager 状态                     │
├─────────────────────────────────────────────────────────────┤
│  _zoom_ratio: float        # 当前缩放比例                    │
│  _view_offset: QPointF     # 视图偏移（片坐标系）             │
│  _tracks: List[TrackInfo]  # track 列表                      │
│  _max_track: TrackInfo     # 当前最大分辨率 track（缓存）     │
├─────────────────────────────────────────────────────────────┤
│                      状态变化触发点                          │
├───────────────────┬─────────────────────────────────────────┤
│ 滚轮缩放          │ → 更新 zoom_ratio，可能更新 view_offset  │
│ ComboBox 缩放     │ → 更新 zoom_ratio，可能更新 view_offset  │
│ 鼠标拖动          │ → 更新 view_offset                       │
│ 窗体 resize       │ → 重新计算 fit，可能钳制 zoom/offset     │
│ 布局模式变化      │ → 更新分割视图，可能钳制                 │
│ 分割比调整        │ → 更新分割视图，可能钳制                 │
│ Add track         │ → 检查 max 变化，可能转换坐标系          │
│ Remove track      │ → 检查 max 变化，可能转换坐标系          │
└───────────────────┴─────────────────────────────────────────┘
```

---

## 15. 实现状态

### 15.1 已实现文件

| 文件路径 | 说明 |
|----------|------|
| `player/ui/widgets/custom_combo_box.py` | 自定义 ComboBox 基类 |
| `player/ui/widgets/zoom_combo_box.py` | 缩放 ComboBox 控件 |
| `player/core/viewport/__init__.py` | Viewport 模块入口 |
| `player/core/viewport/track_layout.py` | 多轨道布局管理 |
| `player/core/viewport/manager.py` | ViewportManager 状态管理 |
| `player/core/actions/viewport_zoom.py` | 缩放 Action |
| `player/core/actions/viewport_pan.py` | 移动 Action |

### 15.2 修改的现有文件

| 文件路径 | 修改内容 |
|----------|----------|
| `player/ui/widgets/__init__.py` | 导出新控件 |
| `player/ui/controls_bar.py` | 使用 ZoomComboBox 替换原有 ComboBox |
| `player/ui/viewport/gl_widget.py` | 添加 viewport 信号 |
| `player/ui/main_window.py` | 集成 ViewportManager |
| `player/core/actions/__init__.py` | 导出新 Actions |
| `player/core/actions/registry.py` | 注册新 Actions |
| `player/core/shortcuts.py` | 更新快捷键映射 |

### 15.3 功能使用说明

**缩放操作**:
- 鼠标滚轮：在 viewport 上滚动滚轮进行缩放
- 缩放 ComboBox：点击下拉选择预设值，或直接输入百分比
- 快捷键：`Ctrl++` 放大，`Ctrl+-` 缩小，`Ctrl+0` 重置到 Fit

**移动操作**:
- 鼠标拖拽：按住中键拖拽移动画面
- 边界钳制：画面不会被移出可视区域（不会出现黑边）

**Fit 值**:
- 当视频加载或窗口大小变化时自动计算
- 缩放值不能小于 Fit 值（确保画面完整显示）

### 15.4 待实现（后续迭代）

- [ ] SpeedComboBox 倍速控件
- [ ] 分割线拖动调整（Split 模式）
- [x] ~~OpenGL shader 中的缩放/偏移渲染~~ ✅ 已实现

### 15.5 Shader 渲染实现 (2025-03-22)

**修改的文件**:
| 文件路径 | 修改内容 |
|----------|----------|
| `player/shaders/multitrack.frag` | 添加 `u_zoom_ratio`, `u_view_offset`, `u_track_sizes` uniform，实现 `calc_aspect_fit_uv` 缩放/偏移计算 |
| `player/ui/viewport/gl_widget.py` | 添加 `_zoom_ratio`, `_view_offset`, `_track_sizes` 状态，`set_viewport_transform()` 和 `set_track_sizes()` 方法，在 `paintGL()` 传递 uniform |
| `player/ui/main_window.py` | 连接 `viewport_changed` 信号到 GLWidget，在 `_update_viewport_tracks()` 中同步 track_sizes |

**渲染逻辑**:
1. ViewportManager 计算缩放比例 (`zoom_ratio`) 和偏移 (`view_offset`)
2. 当状态变化时发出 `viewport_changed` 信号
3. MainWindow 接收信号并调用 `GLWidget.set_viewport_transform()`
4. GLWidget 在 `paintGL()` 中将状态传递给 shader
5. Fragment shader 的 `calc_aspect_fit_uv()` 应用变换计算纹理 UV

**Mock 测试**: `tests/mock/viewport_zoom.vpmock`
