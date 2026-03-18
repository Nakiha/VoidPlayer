"""
VideoPlaceholder - 视频预览占位控件
"""
from PySide6.QtGui import QPainter, QColor
from PySide6.QtWidgets import QWidget

from ...theme_utils import isDarkTheme


class VideoPlaceholder(QWidget):
    """视频预览占位控件 - 后续替换为 GLWidget"""

    def __init__(self, index: int, parent=None):
        super().__init__(parent)
        self.index = index
        self.setMinimumWidth(100)

    def paintEvent(self, event):
        """绘制占位背景"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        # 使用简单的灰度色，便于观察组件分界
        dark = isDarkTheme()
        gray_base = 50 + self.index * 20  # 每个槽位灰度不同
        if dark:
            color = QColor(gray_base, gray_base, gray_base)
        else:
            color = QColor(200 - gray_base, 200 - gray_base, 200 - gray_base)

        painter.fillRect(self.rect(), color)
