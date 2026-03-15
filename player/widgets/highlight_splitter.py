"""
HighlightSplitter - 支持悬浮高亮动画的分割器
"""
from PySide6.QtWidgets import QSplitter, QSplitterHandle, QWidget
from PySide6.QtCore import Property, QPropertyAnimation, QEasingCurve, Qt, QRect
from PySide6.QtGui import QColor, QPainter

from ..theme_utils import get_color, ColorKey, get_accent_color


class _HighlightOverlay(QWidget):
    """高亮覆盖层 - 独立于 handle，可超出 1px 限制"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._color = QColor(Qt.GlobalColor.transparent)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.hide()

    def set_color(self, color: QColor):
        self._color = color
        if color.alpha() > 0:
            self.show()
        else:
            self.hide()
        self.update()

    def paintEvent(self, event):
        if self._color.alpha() > 0:
            painter = QPainter(self)
            painter.fillRect(self.rect(), self._color)


class HighlightSplitterHandle(QSplitterHandle):
    """支持悬浮高亮动画的分割条手柄"""

    HIGHLIGHT_WIDTH = 5  # 高亮区域宽度
    LINE_WIDTH = 1  # 分割线宽度

    def __init__(self, orientation, splitter):
        super().__init__(orientation, splitter)
        self._color = QColor(get_color(ColorKey.BG_BASE))
        self._hover = False
        self._overlay = _HighlightOverlay(splitter)
        self._animation = QPropertyAnimation(self, b"animColor", self)
        self._animation.setDuration(150)
        self._animation.setEasingCurve(QEasingCurve.OutCubic)

    def get_anim_color(self) -> QColor:
        return self._color

    def set_anim_color(self, color: QColor):
        self._color = color
        self._overlay.set_color(color)
        self.update()

    animColor = Property(QColor, get_anim_color, set_anim_color)

    def enterEvent(self, event):
        self._hover = True
        self._animate_to(get_accent_color())
        super().enterEvent(event)

    def leaveEvent(self, event):
        self._hover = False
        self._animate_to(QColor(Qt.GlobalColor.transparent))
        super().leaveEvent(event)

    def _animate_to(self, target: QColor):
        self._animation.stop()
        self._animation.setStartValue(self._color)
        self._animation.setEndValue(target)
        self._animation.start()

    def paintEvent(self, event):
        # 只绘制 1px 分割线
        painter = QPainter(self)
        rect = self.rect()
        bg_color = get_color(ColorKey.BG_BASE)

        if self.orientation() == Qt.Orientation.Horizontal:
            x = (rect.width() - self.LINE_WIDTH) // 2
            painter.fillRect(x, 0, self.LINE_WIDTH, rect.height(), bg_color)
        else:
            y = (rect.height() - self.LINE_WIDTH) // 2
            painter.fillRect(0, y, rect.width(), self.LINE_WIDTH, bg_color)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._update_overlay_geometry()

    def moveEvent(self, event):
        super().moveEvent(event)
        self._update_overlay_geometry()

    def showEvent(self, event):
        super().showEvent(event)
        self._update_overlay_geometry()

    def _update_overlay_geometry(self):
        """更新覆盖层几何位置 - 居中于 handle，宽度为 HIGHLIGHT_WIDTH"""
        handle_rect = self.geometry()

        if self.orientation() == Qt.Orientation.Horizontal:
            # 水平分割器：覆盖层居中于 handle
            handle_center = handle_rect.left() + handle_rect.width() // 2
            x = handle_center - self.HIGHLIGHT_WIDTH // 2
            overlay_rect = QRect(
                x,
                handle_rect.top(),
                self.HIGHLIGHT_WIDTH,
                handle_rect.height()
            )
        else:
            # 垂直分割器：覆盖层居中于 handle
            handle_center = handle_rect.top() + handle_rect.height() // 2
            y = handle_center - self.HIGHLIGHT_WIDTH // 2
            overlay_rect = QRect(
                handle_rect.left(),
                y,
                handle_rect.width(),
                self.HIGHLIGHT_WIDTH
            )

        self._overlay.setGeometry(overlay_rect)


class HighlightSplitter(QSplitter):
    """支持悬浮高亮动画的分割器"""

    def createHandle(self):
        return HighlightSplitterHandle(self.orientation(), self)
