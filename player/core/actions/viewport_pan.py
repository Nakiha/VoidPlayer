"""
ViewportPanAction - Viewport 画面移动 Action

触发方式：鼠标拖拽（在 viewport 上）
"""
from typing import TYPE_CHECKING

from PySide6.QtCore import QObject, QPointF, Signal

if TYPE_CHECKING:
    from player.core.viewport import ViewportManager


class ViewportPanAction(QObject):
    """Viewport 画面移动 Action

    触发方式：鼠标拖拽（在 viewport 上）
    """

    ACTION_ID = "viewport_pan"

    # 信号
    pan_started = Signal()
    pan_finished = Signal()
    offset_changed = Signal(QPointF)

    def __init__(self, viewport_manager: "ViewportManager", parent=None):
        super().__init__(parent)
        self._viewport_manager = viewport_manager
        self._is_panning = False
        self._last_pos: QPointF = QPointF()

    @property
    def is_panning(self) -> bool:
        return self._is_panning

    def on_mouse_press(self, x: float, y: float):
        """开始拖拽

        Args:
            x: 鼠标 x 坐标
            y: 鼠标 y 坐标
        """
        self._is_panning = True
        self._last_pos = QPointF(x, y)
        self.pan_started.emit()

    def on_mouse_move(self, x: float, y: float):
        """拖拽移动

        Args:
            x: 鼠标 x 坐标
            y: 鼠标 y 坐标
        """
        if not self._is_panning:
            return

        current_pos = QPointF(x, y)
        delta = current_pos - self._last_pos
        self._last_pos = current_pos

        # 直接应用，无动画
        self._viewport_manager.apply_pan(delta)
        self.offset_changed.emit(self._viewport_manager.view_offset)

    def on_mouse_release(self):
        """结束拖拽"""
        if self._is_panning:
            self._is_panning = False
            self._last_pos = QPointF()
            self.pan_finished.emit()

    def cancel(self):
        """取消当前拖拽"""
        self._is_panning = False
        self._last_pos = QPointF()
