"""
ViewportZoomAction - Viewport 缩放 Action

触发方式：
1. 鼠标滚轮（在 viewport 上）
2. ControlsBar 的 ZoomComboBox 编辑/选择
3. 快捷键 ZOOM_IN / ZOOM_OUT / ZOOM_RESET
"""
from typing import TYPE_CHECKING

from PySide6.QtCore import QObject, Signal

if TYPE_CHECKING:
    from player.core.viewport import ViewportManager


class ViewportZoomAction(QObject):
    """Viewport 缩放 Action

    触发方式：
    1. 鼠标滚轮（在 viewport 上）
    2. ControlsBar 的 ZoomComboBox 编辑/选择
    3. 快捷键 ZOOM_IN / ZOOM_OUT / ZOOM_RESET
    """

    ACTION_ID = "viewport_zoom"

    # 信号
    zoom_changed = Signal(float)  # 缩放比例变化

    def __init__(self, viewport_manager: "ViewportManager", parent=None):
        super().__init__(parent)
        self._viewport_manager = viewport_manager

    def on_wheel(self, delta: int, mouse_widget_x: float, mouse_widget_y: float):
        """处理滚轮事件

        Args:
            delta: 滚轮增量（正=上滚，负=下滚）
            mouse_widget_x: 鼠标在 QOpenGLWidget 中的 x 坐标
            mouse_widget_y: 鼠标在 QOpenGLWidget 中的 y 坐标
        """
        from PySide6.QtCore import QPointF
        mouse_pos = QPointF(mouse_widget_x, mouse_widget_y)
        self._viewport_manager.apply_wheel_zoom(delta, mouse_pos)
        self.zoom_changed.emit(self._viewport_manager.zoom_ratio)

    def on_zoom_value_changed(self, zoom_ratio: float):
        """处理 ComboBox 值变化"""
        self._viewport_manager.apply_zoom_ratio(zoom_ratio)
        self.zoom_changed.emit(self._viewport_manager.zoom_ratio)

    def zoom_in(self):
        """放大（快捷键触发）"""
        from PySide6.QtCore import QPointF
        # 使用中心缩放
        self._viewport_manager.apply_wheel_zoom(120, QPointF())
        self.zoom_changed.emit(self._viewport_manager.zoom_ratio)

    def zoom_out(self):
        """缩小（快捷键触发）"""
        from PySide6.QtCore import QPointF
        # 使用中心缩放
        self._viewport_manager.apply_wheel_zoom(-120, QPointF())
        self.zoom_changed.emit(self._viewport_manager.zoom_ratio)

    def zoom_reset(self):
        """重置缩放（快捷键触发）"""
        self._viewport_manager.reset_zoom()
        self.zoom_changed.emit(self._viewport_manager.zoom_ratio)
