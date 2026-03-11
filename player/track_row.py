"""
TrackRow - 单条轨道控制
"""
from PySide6.QtWidgets import QWidget, QHBoxLayout, QLabel, QFrame
from PySide6.QtCore import Signal, Qt
from PySide6.QtGui import QPainter, QColor, QPen, QFont
from qfluentwidgets import (
    TransparentToolButton,
    BodyLabel,
    FluentIcon,
)

from .theme_utils import get_color, get_color_hex, get_accent_color
from .widgets import create_tool_button


class TrackContent(QWidget):
    """轨道内容区 - 显示视频片段和播放头"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._playhead_position = 0.3  # 播放头位置 (0~1)
        self._clip_width = 0.9  # 视频片段宽度比例
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

        # 背景
        painter.fillRect(self.rect(), get_color("bg_track_content"))

        # 绘制视频片段
        clip_margin = 5
        clip_height = 26
        clip_y = (self.height() - clip_height) // 2
        clip_width = int((self.width() - clip_margin * 2) * self._clip_width)

        painter.setBrush(get_color("bg_clip"))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(clip_margin, clip_y, clip_width, clip_height, 2, 2)

        # 绘制播放头 (使用系统主题色)
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

    def __init__(self, file_name: str = "", parent=None):
        super().__init__(parent)
        self._file_name = file_name
        self._is_visible = True
        self._is_muted = False
        self._offset_ms = 0
        self.setFixedHeight(40)
        self._setup_ui()

    def _setup_ui(self):
        main_layout = QHBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # 左侧控制区 (320px)
        self.controls_panel = QWidget(self)
        self.controls_panel.setFixedWidth(320)
        self._update_controls_style()
        controls_layout = QHBoxLayout(self.controls_panel)
        controls_layout.setContentsMargins(12, 0, 12, 0)
        controls_layout.setSpacing(10)

        # 移除按钮
        self.remove_btn = create_tool_button(FluentIcon.CLOSE, self, 20)
        self.remove_btn.setToolTip("移除轨道")
        self.remove_btn.clicked.connect(self.remove_clicked)
        controls_layout.addWidget(self.remove_btn)

        # 文件名
        self.file_label = BodyLabel(self._file_name, self)
        controls_layout.addWidget(self.file_label, 1)

        # 可见性按钮
        self.visibility_btn = create_tool_button(FluentIcon.VIEW, self, 20)
        self.visibility_btn.setToolTip("显示/隐藏视频")
        self.visibility_btn.clicked.connect(self._on_visibility_clicked)
        controls_layout.addWidget(self.visibility_btn)

        # 静音按钮
        self.mute_btn = create_tool_button(FluentIcon.VOLUME, self, 20)
        self.mute_btn.setToolTip("静音/取消静音")
        self.mute_btn.clicked.connect(self._on_mute_clicked)
        controls_layout.addWidget(self.mute_btn)

        # 偏移控制区
        # 后退按钮
        self.offset_prev_btn = create_tool_button(FluentIcon.LEFT_ARROW, self, 18)
        self.offset_prev_btn.setToolTip("偏移 -1 帧")
        self.offset_prev_btn.clicked.connect(self._on_offset_decrease)
        controls_layout.addWidget(self.offset_prev_btn)

        # 偏移时间显示
        self.offset_label = BodyLabel("00:00", self)
        # 使用 QFont 设置字体，避免与 qfluentwidgets 主题系统冲突
        font = QFont("Consolas, Monaco, monospace")
        font.setPointSize(10)
        self.offset_label.setFont(font)
        self.offset_label.setStyleSheet(f"color: {get_color_hex('text_secondary')};")
        self.offset_label.setFixedWidth(45)
        controls_layout.addWidget(self.offset_label)

        # 前进按钮
        self.offset_next_btn = create_tool_button(FluentIcon.RIGHT_ARROW, self, 18)
        self.offset_next_btn.setToolTip("偏移 +1 帧")
        self.offset_next_btn.clicked.connect(self._on_offset_increase)
        controls_layout.addWidget(self.offset_next_btn)

        main_layout.addWidget(self.controls_panel)

        # 右侧轨道区
        self.track_content = TrackContent(self)
        main_layout.addWidget(self.track_content, 1)

    def _update_controls_style(self):
        """更新控制区样式"""
        self.controls_panel.setStyleSheet(
            f"background-color: {get_color_hex('bg_track_controls')};"
        )

    def _on_visibility_clicked(self):
        """可见性切换"""
        self._is_visible = not self._is_visible
        # 更新图标
        if self._is_visible:
            self.visibility_btn.setIcon(FluentIcon.VIEW)
        else:
            self.visibility_btn.setIcon(FluentIcon.VIEW_OFF)
        self.visibility_toggled.emit(self._is_visible)

    def _on_mute_clicked(self):
        """静音切换"""
        self._is_muted = not self._is_muted
        # 更新图标
        if self._is_muted:
            self.mute_btn.setIcon(FluentIcon.MUTE)
        else:
            self.mute_btn.setIcon(FluentIcon.VOLUME)
        self.mute_toggled.emit(self._is_muted)

    def _on_offset_decrease(self):
        """减少偏移 (-1帧 ≈ 33ms)"""
        self._offset_ms -= 33
        self._update_offset_display()
        self.offset_changed.emit(self._offset_ms)

    def _on_offset_increase(self):
        """增加偏移 (+1帧 ≈ 33ms)"""
        self._offset_ms += 33
        self._update_offset_display()
        self.offset_changed.emit(self._offset_ms)

    def _update_offset_display(self):
        """更新偏移时间显示"""
        abs_ms = abs(self._offset_ms)
        sec = abs_ms // 1000
        ms = abs_ms % 1000 // 10
        sign = "-" if self._offset_ms < 0 else ""
        self.offset_label.setText(f"{sign}{sec:02d}:{ms:02d}")

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
        self._update_offset_display()
