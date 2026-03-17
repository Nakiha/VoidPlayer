"""
DraggableElideLabel - 支持拖拽的省略文本标签
"""
from PySide6.QtCore import Signal, Qt, QPoint
from PySide6.QtGui import QMouseEvent, QEnterEvent

from .elide_label import ElideLabel

# 拖拽触发阈值 (像素)
DRAG_THRESHOLD = 4


class DraggableElideLabel(ElideLabel):
    """支持拖拽的省略文本标签 - 悬停显示手掌光标，支持拖拽重排序"""

    # 信号
    drag_started = Signal()  # 开始拖拽
    drag_moved = Signal(int)  # 拖拽移动 (global_y)
    drag_finished = Signal()  # 结束拖拽

    def __init__(self, text: str = "", parent=None):
        super().__init__(text, parent)
        self._dragging = False
        self._start_pos = QPoint()
        self._pressed = False

    def enterEvent(self, event: QEnterEvent):
        """悬停时显示手掌光标"""
        super().enterEvent(event)
        self.setCursor(Qt.CursorShape.OpenHandCursor)

    def leaveEvent(self, event):
        """离开时恢复默认光标"""
        super().leaveEvent(event)
        if not self._dragging:
            self.setCursor(Qt.CursorShape.ArrowCursor)

    def mousePressEvent(self, event: QMouseEvent):
        """按下时记录起始位置"""
        if event.button() == Qt.MouseButton.LeftButton:
            self._pressed = True
            self._start_pos = event.globalPosition().toPoint()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event: QMouseEvent):
        """移动时检测是否开始拖拽"""
        if self._pressed and not self._dragging:
            current_pos = event.globalPosition().toPoint()
            distance = (current_pos - self._start_pos).manhattanLength()
            if distance > DRAG_THRESHOLD:
                # 开始拖拽
                self._dragging = True
                self.drag_started.emit()

        if self._dragging:
            # 持续发送拖拽位置
            global_y = event.globalPosition().toPoint().y()
            self.drag_moved.emit(global_y)

        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event: QMouseEvent):
        """释放时结束拖拽"""
        if self._dragging:
            self.drag_finished.emit()

        self._dragging = False
        self._pressed = False

        # 恢复光标
        if self.rect().contains(event.position().toPoint()):
            self.setCursor(Qt.CursorShape.OpenHandCursor)
        else:
            self.setCursor(Qt.CursorShape.ArrowCursor)

        super().mouseReleaseEvent(event)

    def is_dragging(self) -> bool:
        """返回是否正在拖拽"""
        return self._dragging
