"""
拖动调整控件
"""
from PySide6.QtGui import QCursor, QPainter
from PySide6.QtWidgets import QWidget, QVBoxLayout
from PySide6.QtCore import Qt, QPoint, Signal, QEvent

from player.theme_utils import get_accent_color


class _EdgeOverlay(QWidget):
    """内部覆盖层 - 用于在子控件之上绘制边缘高亮"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._edge = 0
        self._edge_size = 6
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)

    def set_edge(self, edge: int, size: int):
        """设置要高亮的边缘"""
        self._edge = edge
        self._edge_size = size
        self.update()

    def paintEvent(self, event):
        """绘制边缘高亮"""
        if not self._edge:
            return

        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        accent = get_accent_color()

        if self._edge & ResizableContainer.Edge.LEFT:
            painter.fillRect(0, 0, self._edge_size, self.height(), accent)
        if self._edge & ResizableContainer.Edge.RIGHT:
            painter.fillRect(self.width() - self._edge_size, 0, self._edge_size, self.height(), accent)
        if self._edge & ResizableContainer.Edge.TOP:
            painter.fillRect(0, 0, self.width(), self._edge_size, accent)
        if self._edge & ResizableContainer.Edge.BOTTOM:
            painter.fillRect(0, self.height() - self._edge_size, self.width(), self._edge_size, accent)


class ResizableContainer(QWidget):
    """
    可拖动边缘的透明容器

    包裹子控件，在指定边缘提供拖动调整功能。
    默认透明，仅在可拖动边缘悬停时显示高亮。

    支持宽度边界锁定：当宽度达到 min/max 限制后，用户需要将鼠标
    "回弹"到首次触发边界的位置，宽度才会开始变化。

    Example:
        container = ResizableContainer()
        container.setResizable(ResizableContainer.Edge.LEFT)
        container.setRange(200, 600)
        container.setCurrentWidth(320)
        container.widthChanged.connect(self._on_width_changed)
        container.setWidget(my_widget)
    """

    class Edge:
        """边缘枚举"""
        LEFT = 1
        RIGHT = 2
        TOP = 4
        BOTTOM = 8

    # 信号
    widthChanged = Signal(int)  # 宽度变化（已应用边界锁定逻辑）
    drag_started = Signal()
    drag_finished = Signal()

    # 默认宽度限制
    DEFAULT_MIN_WIDTH = 200
    DEFAULT_MAX_WIDTH = 600

    def __init__(self, parent=None):
        super().__init__(parent)
        self._edge_mask = 0
        self._edge_size = 6
        self._child_widget = None
        self._dragging = False
        self._drag_edge = 0
        self._start_pos = QPoint()
        self._hover_edge = 0
        self._hovering_child = False

        # 宽度控制
        self._min_width = self.DEFAULT_MIN_WIDTH
        self._max_width = self.DEFAULT_MAX_WIDTH
        self._current_width = 320  # 当前宽度
        self._drag_start_width = 0  # 拖动开始时的宽度
        self._drag_locked_offset = None  # 边界锁定偏移

        self.setMouseTracking(True)
        self._setup_ui()

    def _setup_ui(self):
        """设置布局"""
        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(0, 0, 0, 0)
        self._layout.setSpacing(0)

        # 覆盖层用于绘制边缘高亮（在子控件之上）
        self._overlay = _EdgeOverlay(self)
        self._overlay.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self._overlay.hide()

    def resizeEvent(self, event):
        """调整大小时同步覆盖层"""
        super().resizeEvent(event)
        self._overlay.setGeometry(self.rect())

    def _update_overlay(self):
        """更新覆盖层显示"""
        if self._hover_edge:
            self._overlay.set_edge(self._hover_edge, self._edge_size)
            self._overlay.raise_()
            self._overlay.show()
        else:
            self._overlay.hide()

    def setResizable(self, edges: int):
        """设置可拖动的边缘"""
        self._edge_mask = edges

    def setEdgeSize(self, size: int):
        """设置边缘触发区域大小"""
        self._edge_size = size

    def setRange(self, min_width: int, max_width: int):
        """设置宽度范围"""
        self._min_width = min_width
        self._max_width = max_width

    def setCurrentWidth(self, width: int):
        """设置当前宽度"""
        self._current_width = width

    def currentWidth(self) -> int:
        """获取当前宽度"""
        return self._current_width

    def minWidth(self) -> int:
        """获取最小宽度"""
        return self._min_width

    def maxWidth(self) -> int:
        """获取最大宽度"""
        return self._max_width

    def setWidget(self, widget: QWidget):
        """设置子控件"""
        if self._child_widget:
            self._child_widget.removeEventFilter(self)
            self._child_widget.deleteLater()
        self._child_widget = widget
        self._child_widget.installEventFilter(self)
        self._child_widget.setMouseTracking(True)
        self._layout.addWidget(widget)

    def _get_edge_at(self, pos: QPoint) -> int:
        """获取指定位置所在的边缘"""
        edge = 0
        x, y = pos.x(), pos.y()

        if self._edge_mask & self.Edge.LEFT and x <= self._edge_size:
            edge |= self.Edge.LEFT
        if self._edge_mask & self.Edge.RIGHT and x >= self.width() - self._edge_size:
            edge |= self.Edge.RIGHT
        if self._edge_mask & self.Edge.TOP and y <= self._edge_size:
            edge |= self.Edge.TOP
        if self._edge_mask & self.Edge.BOTTOM and y >= self.height() - self._edge_size:
            edge |= self.Edge.BOTTOM

        return edge

    def _calculate_width_with_lock(self, total_x: int) -> int:
        """
        计算新宽度（带边界锁定逻辑）

        Args:
            total_x: 相对于拖动开始位置的总偏移

        Returns:
            计算后的新宽度
        """
        theoretical_width = self._drag_start_width + total_x

        # 检查是否超出边界
        if theoretical_width <= self._min_width:
            self._drag_locked_offset = self._min_width - self._drag_start_width
            return self._min_width
        elif theoretical_width >= self._max_width:
            self._drag_locked_offset = self._max_width - self._drag_start_width
            return self._max_width
        else:
            # 在边界范围内
            if self._drag_locked_offset is not None:
                # 检查是否已回弹到或超过锁定点
                should_stay_locked = (
                    (self._drag_locked_offset > 0 and total_x > self._drag_locked_offset) or
                    (self._drag_locked_offset < 0 and total_x < self._drag_locked_offset)
                )
                if should_stay_locked:
                    return self._min_width if self._drag_locked_offset < 0 else self._max_width
                else:
                    self._drag_locked_offset = None
            return theoretical_width

    def _on_drag_start(self):
        """拖动开始"""
        self._drag_start_width = self._current_width
        self._drag_locked_offset = None

    def _on_drag_move(self, total_x: int):
        """拖动移动"""
        new_width = self._calculate_width_with_lock(total_x)
        if new_width != self._current_width:
            self._current_width = int(new_width)
            self.widthChanged.emit(self._current_width)

    def eventFilter(self, obj, event):
        """事件过滤器 - 捕获子控件的鼠标事件"""
        if obj == self._child_widget:
            event_type = event.type()

            if event_type == QEvent.Type.MouseMove:
                child_pos = event.position().toPoint()
                global_pos = self._child_widget.mapToGlobal(child_pos)
                container_pos = self.mapFromGlobal(global_pos)

                if self._dragging:
                    current_pos = event.globalPosition().toPoint()
                    total_x = current_pos.x() - self._start_pos.x()
                    self._on_drag_move(total_x)
                else:
                    edge = self._get_edge_at(container_pos)
                    if edge != self._hover_edge:
                        self._hover_edge = edge
                        self._update_overlay()
                    self._child_widget.setCursor(self._get_cursor_for_edge(edge))

            elif event_type == QEvent.Type.MouseButtonPress:
                if event.button() == Qt.MouseButton.LeftButton:
                    child_pos = event.position().toPoint()
                    global_pos = self._child_widget.mapToGlobal(child_pos)
                    container_pos = self.mapFromGlobal(global_pos)
                    edge = self._get_edge_at(container_pos)

                    if edge:
                        self._dragging = True
                        self._drag_edge = edge
                        self._start_pos = event.globalPosition().toPoint()
                        self._on_drag_start()
                        self.drag_started.emit()
                        return True

            elif event_type == QEvent.Type.MouseButtonRelease:
                if event.button() == Qt.MouseButton.LeftButton and self._dragging:
                    self._dragging = False
                    self._drag_edge = 0
                    self.drag_finished.emit()
                    return True

            elif event_type == QEvent.Type.Leave:
                self._hover_edge = 0
                self._update_overlay()

        return super().eventFilter(obj, event)

    def _get_cursor_for_edge(self, edge: int) -> QCursor:
        """根据边缘获取光标形状"""
        if edge & self.Edge.LEFT and edge & self.Edge.TOP:
            return QCursor(Qt.CursorShape.SizeFDiagCursor)
        elif edge & self.Edge.RIGHT and edge & self.Edge.BOTTOM:
            return QCursor(Qt.CursorShape.SizeFDiagCursor)
        elif edge & self.Edge.RIGHT and edge & self.Edge.TOP:
            return QCursor(Qt.CursorShape.SizeBDiagCursor)
        elif edge & self.Edge.LEFT and edge & self.Edge.BOTTOM:
            return QCursor(Qt.CursorShape.SizeBDiagCursor)
        elif edge & (self.Edge.LEFT | self.Edge.RIGHT):
            return QCursor(Qt.CursorShape.SplitHCursor)
        elif edge & (self.Edge.TOP | self.Edge.BOTTOM):
            return QCursor(Qt.CursorShape.SplitVCursor)
        else:
            return QCursor(Qt.CursorShape.ArrowCursor)

    def mouseMoveEvent(self, event):
        """鼠标移动"""
        pos = event.pos()

        if self._dragging:
            current_pos = event.globalPosition().toPoint()
            total_x = current_pos.x() - self._start_pos.x()
            self._on_drag_move(total_x)
        else:
            edge = self._get_edge_at(pos)
            if edge != self._hover_edge:
                self._hover_edge = edge
                self._update_overlay()
            self.setCursor(self._get_cursor_for_edge(edge))

        super().mouseMoveEvent(event)

    def mousePressEvent(self, event):
        """鼠标按下"""
        if event.button() == Qt.MouseButton.LeftButton:
            edge = self._get_edge_at(event.pos())
            if edge:
                self._dragging = True
                self._drag_edge = edge
                self._start_pos = event.globalPosition().toPoint()
                self._on_drag_start()
                self.drag_started.emit()

    def mouseReleaseEvent(self, event):
        """鼠标释放"""
        if event.button() == Qt.MouseButton.LeftButton and self._dragging:
            self._dragging = False
            self._drag_edge = 0
            self.drag_finished.emit()

    def leaveEvent(self, event):
        """鼠标离开"""
        self._hover_edge = 0
        self._update_overlay()
        super().leaveEvent(event)
