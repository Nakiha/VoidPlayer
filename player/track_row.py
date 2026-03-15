"""
TrackRow - 单条轨道控制
"""
from PySide6.QtWidgets import QWidget, QHBoxLayout, QLabel, QFrame
from PySide6.QtCore import Signal, Qt
from PySide6.QtGui import QPainter, QPen
from qfluentwidgets import (
    TransparentToolButton,
    BodyLabel,
    FluentIcon,
)

from .theme_utils import get_color, get_color_hex, get_accent_color, ColorKey
from .widgets import create_tool_button, OffsetLabel, HighlightSplitter


class TrackContent(QWidget):
    """轨道内容区 - 显示视频片段和播放头"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._playhead_position = 0.3
        self._clip_width = 0.9
        self.setMinimumWidth(100)

    def set_playhead_position(self, position: float):
        """设置播放头位置 (0~1)"""
        self._playhead_position = max(0, min(1, position))
        self.update()

    def set_clip_width(self, width: float):
        """设置视频片段宽度比例"""
        self._clip_width = max(0, min(1, width))
        self.update()

    def paintEvent(self, event):
        """绘制轨道内容"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        # 绘制视频片段
        clip_margin = 5
        clip_height = 26
        clip_y = (self.height() - clip_height) // 2
        clip_width = int((self.width() - clip_margin * 2) * self._clip_width)

        painter.setBrush(get_color(ColorKey.BG_CLIP))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(clip_margin, clip_y, clip_width, clip_height, 2, 2)

        # 绘制播放头
        playhead_x = int(self.width() * self._playhead_position)
        painter.setPen(QPen(get_accent_color(), 1))
        painter.drawLine(playhead_x, 0, playhead_x, self.height())


class TrackRow(QWidget):
    """单条轨道 - 控制区和轨道区"""

    # 信号
    remove_clicked = Signal()
    visibility_toggled = Signal(bool)
    mute_toggled = Signal(bool)
    offset_changed = Signal(int)  # 毫秒
    controls_width_changed = Signal(int)  # 请求调整 controls_panel 宽度

    # 类级别的默认宽度（所有 TrackRow 共享）
    _controls_width = 320

    def __init__(self, file_name: str = "", parent=None):
        super().__init__(parent)
        self._file_name = file_name
        self._is_visible = True
        self._is_muted = False
        self._offset_ms = 0
        self._syncing = False  # 防止递归同步
        self.setFixedHeight(40)
        self._setup_ui()

    def _setup_ui(self):
        # 设置 TrackRow 支持 QSS 背景色
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)

        main_layout = QHBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # 使用 splitter 分割控制区和轨道区
        self.splitter = HighlightSplitter(Qt.Orientation.Horizontal, self)
        self.splitter.setHandleWidth(1)
        self.splitter.setChildrenCollapsible(False)

        # 左侧控制区 - 透明背景
        self.controls_panel = QWidget(self.splitter)
        self.controls_panel.setMinimumWidth(200)  # 最小宽度限制
        self.controls_panel.setStyleSheet("background: transparent;")
        controls_layout = QHBoxLayout(self.controls_panel)
        controls_layout.setContentsMargins(8, 4, 8, 4)
        controls_layout.setSpacing(4)

        # 移除按钮
        self.remove_btn = create_tool_button(FluentIcon.CLOSE, self, 28)
        self.remove_btn.setToolTip("移除轨道")
        self.remove_btn.clicked.connect(self.remove_clicked)
        controls_layout.addWidget(self.remove_btn)

        # 文件名
        self.file_label = BodyLabel(self._file_name, self)
        self.file_label.setStyleSheet("BodyLabel { background: transparent; }")
        controls_layout.addWidget(self.file_label, 1)

        # 可见性按钮
        self.visibility_btn = create_tool_button(FluentIcon.VIEW, self, 28)
        self.visibility_btn.setToolTip("显示/隐藏视频")
        self.visibility_btn.clicked.connect(self._on_visibility_clicked)
        controls_layout.addWidget(self.visibility_btn)

        # 静音按钮
        self.mute_btn = create_tool_button(FluentIcon.VOLUME, self, 28)
        self.mute_btn.setToolTip("静音/取消静音")
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        controls_layout.addWidget(self.mute_btn)

        # 偏移控制区
        self.offset_prev_btn = create_tool_button(FluentIcon.LEFT_ARROW, self, 28)
        self.offset_prev_btn.setToolTip("偏移 -1 帧")
        self.offset_prev_btn.clicked.connect(self._on_offset_decrease)
        controls_layout.addWidget(self.offset_prev_btn)

        self.offset_next_btn = create_tool_button(FluentIcon.RIGHT_ARROW, self, 28)
        self.offset_next_btn.setToolTip("偏移 +0.01秒")
        self.offset_next_btn.clicked.connect(self._on_offset_increase)
        controls_layout.addWidget(self.offset_next_btn)

        self.offset_label = OffsetLabel(self)
        controls_layout.addWidget(self.offset_label)

        self.splitter.addWidget(self.controls_panel)

        # 右侧轨道区
        self.track_content = TrackContent(self.splitter)
        self.splitter.addWidget(self.track_content)

        # 初始化分割位置
        self.splitter.setSizes([TrackRow._controls_width, 1000])

        # 连接分割器移动信号
        self.splitter.splitterMoved.connect(self._on_splitter_moved)

        main_layout.addWidget(self.splitter, 1)

    def set_alt_row(self, is_alt: bool):
        """设置是否为交替行"""
        bg_color = ColorKey.BG_TRACK_ALT if is_alt else ColorKey.BG_TRACK_CONTROLS
        self.setStyleSheet(f"TrackRow {{ background-color: {get_color_hex(bg_color)}; }}")

    def _on_visibility_clicked(self):
        """可见性切换"""
        self._is_visible = not self._is_visible
        if self._is_visible:
            self.visibility_btn.setIcon(FluentIcon.VIEW)
        else:
            self.visibility_btn.setIcon(FluentIcon.HIDE)
        self.visibility_toggled.emit(self._is_visible)

    def _on_mute_clicked(self):
        """静音切换"""
        self._is_muted = not self._is_muted
        if self._is_muted:
            self.mute_btn.setIcon(FluentIcon.MUTE)
        else:
            self.mute_btn.setIcon(FluentIcon.VOLUME)
        self.mute_toggled.emit(self._is_muted)

    def _on_offset_decrease(self):
        """减少偏移"""
        self._offset_ms -= 10
        self.offset_label.setOffset(self._offset_ms)
        self.offset_changed.emit(self._offset_ms)

    def _on_offset_increase(self):
        """增加偏移"""
        self._offset_ms += 10
        self.offset_label.setOffset(self._offset_ms)
        self.offset_changed.emit(self._offset_ms)

    def _on_splitter_moved(self, pos: int, index: int):
        """分割器移动 - 通知 TimelineArea 同步所有轨道"""
        if self._syncing:
            return

        sizes = self.splitter.sizes()
        total_width = sum(sizes)
        max_width = int(total_width * 0.6)

        new_width = sizes[0]
        if new_width > max_width:
            # 限制在 60% 以内
            new_width = max_width
            sizes[0] = new_width
            sizes[1] = total_width - new_width
            self._syncing = True
            self.splitter.setSizes(sizes)
            self._syncing = False

        self.controls_width_changed.emit(new_width)

    def set_file_name(self, name: str):
        """设置文件名"""
        self._file_name = name
        self.file_label.setText(name)

    def set_playhead_position(self, position: float):
        """设置播放头位置"""
        self.track_content.set_playhead_position(position)

    def set_offset(self, offset_ms: int):
        """设置偏移值"""
        self._offset_ms = offset_ms
        self.offset_label.setOffset(self._offset_ms)

    @classmethod
    def set_controls_width(cls, width: int):
        """设置类级别的 controls_panel 宽度"""
        cls._controls_width = width

    def apply_controls_width(self, width: int):
        """应用宽度到当前 TrackRow 的 controls_panel"""
        if self._syncing:
            return
        self._syncing = True
        self.splitter.moveSplitter(width, 1)
        self._syncing = False
