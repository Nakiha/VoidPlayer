"""
ViewportPanel - 视频预览区域
"""
from PySide6.QtWidgets import QWidget, QHBoxLayout, QSplitter
from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QPainter, QLinearGradient

from .view_mode import ViewMode
from .theme_utils import get_color, get_color_hex, isDarkTheme


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

        # 绘制渐变背景模拟视频画面
        gradient = QLinearGradient(0, 0, self.width(), self.height())

        dark = isDarkTheme()
        if dark:
            if self.index == 0:
                gradient.setColorAt(0, QColor(68, 68, 68))
                gradient.setColorAt(1, QColor(34, 34, 34))
            else:
                gradient.setColorAt(0, QColor(51, 51, 51))
                gradient.setColorAt(1, QColor(17, 17, 17))
        else:
            if self.index == 0:
                gradient.setColorAt(0, QColor(180, 180, 180))
                gradient.setColorAt(1, QColor(140, 140, 140))
            else:
                gradient.setColorAt(0, QColor(160, 160, 160))
                gradient.setColorAt(1, QColor(120, 120, 120))

        painter.fillRect(self.rect(), gradient)


class ViewportPanel(QWidget):
    """视频预览区域 - 双视频并排显示"""

    # 信号
    split_position_changed = Signal(float)  # 分割线位置变化

    def __init__(self, parent=None):
        super().__init__(parent)
        self._view_mode = ViewMode.SIDE_BY_SIDE
        self._split_position = 0.5
        self._setup_ui()

    def _setup_ui(self):
        self.main_layout = QHBoxLayout(self)
        self.main_layout.setContentsMargins(0, 0, 0, 0)
        self.main_layout.setSpacing(0)

        # 创建分割器用于分屏模式
        self.splitter = QSplitter(Qt.Orientation.Horizontal, self)
        self.splitter.setHandleWidth(1)
        self.splitter.setChildrenCollapsible(False)

        # 创建两个视频预览占位控件
        self.video_left = VideoPlaceholder(0, self)
        self.video_right = VideoPlaceholder(1, self)

        self.splitter.addWidget(self.video_left)
        self.splitter.addWidget(self.video_right)

        # 初始化为 50% 分割
        self.splitter.setSizes([1000, 1000])

        self.main_layout.addWidget(self.splitter)

        # 监听分割器变化
        self.splitter.splitterMoved.connect(self._on_splitter_moved)

    def _on_splitter_moved(self, pos: int, index: int):
        """分割器位置变化"""
        total = self.splitter.width()
        if total > 0:
            self._split_position = pos / total
            self.split_position_changed.emit(self._split_position)

    def set_view_mode(self, mode: ViewMode):
        """设置视图模式"""
        self._view_mode = mode
        if mode == ViewMode.SIDE_BY_SIDE:
            # 并排模式: 固定 50% 分割
            self.splitter.setSizes([self.width() // 2, self.width() // 2])
        # 分屏模式: 保持当前位置或使用记录的位置

    @property
    def view_mode(self) -> ViewMode:
        return self._view_mode

    @property
    def split_position(self) -> float:
        return self._split_position

    @split_position.setter
    def split_position(self, value: float):
        self._split_position = max(0.1, min(0.9, value))
        total = self.splitter.width()
        if total > 0:
            left = int(total * self._split_position)
            self.splitter.setSizes([left, total - left])
