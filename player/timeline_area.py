"""
TimelineArea - 时间轴轨道区域
"""
from typing import Optional
from PySide6.QtWidgets import QWidget, QVBoxLayout, QSizePolicy
from PySide6.QtCore import Signal, QTimer, Qt
from qfluentwidgets import SmoothScrollArea

from .track_row import TrackRow
from .theme_utils import get_color_hex, ColorKey


class TimelineArea(QWidget):
    """时间轴轨道区域 - 管理多条轨道"""

    # 信号 (转发 TrackRow 的信号)
    track_remove_clicked = Signal(int)  # index
    track_visibility_toggled = Signal(int, bool)  # index, visible
    track_mute_toggled = Signal(int, bool)  # index, muted
    track_offset_changed = Signal(int, int)  # index, offset_ms
    expand_requested = Signal(int)  # 请求扩展高度 (所需高度)

    MAX_CONTROLS_RATIO = 0.6  # controls_panel 最大占比
    TRACK_ROW_HEIGHT = 40  # 单条轨道高度

    def __init__(self, parent=None):
        super().__init__(parent)
        self._tracks: list[TrackRow] = []
        self._playhead_position = 0.0
        self._controls_width = 320  # 当前 controls_panel 宽度（像素）
        self._syncing = False  # 防止递归同步
        self._setup_ui()

    def _setup_ui(self):
        self.setStyleSheet(f"background-color: {get_color_hex(ColorKey.BG_TIMELINE)};")

        # 设置 sizePolicy 让高度根据内容自动调整
        self.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Preferred)

        # 主布局
        self.main_layout = QVBoxLayout(self)
        self.main_layout.setContentsMargins(0, 0, 0, 0)
        self.main_layout.setSpacing(0)

        # 滚动区域
        self.scroll_area = SmoothScrollArea(self)
        self.scroll_area.setWidgetResizable(True)
        self.scroll_area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.scroll_area.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.scroll_area.setStyleSheet(f"""
            SmoothScrollArea {{
                background-color: {get_color_hex(ColorKey.BG_TIMELINE)};
                border: none;
            }}
            QScrollBar:vertical {{
                width: 8px;
                background-color: transparent;
            }}
            QScrollBar::handle:vertical {{
                background-color: {get_color_hex(ColorKey.BG_TRACK_ALT)};
                border-radius: 4px;
                min-height: 20px;
            }}
            QScrollBar::handle:vertical:hover {{
                background-color: {get_color_hex(ColorKey.TEXT_SECONDARY)};
            }}
        """)

        # 滚动区域内容容器
        self.scroll_content = QWidget(self.scroll_area)
        self.scroll_content.setStyleSheet(f"background-color: {get_color_hex(ColorKey.BG_TIMELINE)};")
        self.tracks_layout = QVBoxLayout(self.scroll_content)
        self.tracks_layout.setContentsMargins(0, 0, 0, 0)
        self.tracks_layout.setSpacing(0)

        self.scroll_area.setWidget(self.scroll_content)
        self.main_layout.addWidget(self.scroll_area)

        # 更新最大高度
        self._update_max_height()

    def add_track(self, index: int, name: str):
        """添加轨道"""
        # 确保 index 在有效范围内
        while len(self._tracks) < index:
            self._tracks.append(None)

        # 从现有 track 获取当前实际宽度（解决窗口 resize 后宽度不同步的问题）
        self._sync_controls_width()

        track = TrackRow(name, self)
        track.remove_clicked.connect(lambda: self._on_track_remove(index))
        track.visibility_toggled.connect(lambda v: self._on_track_visibility(index, v))
        track.mute_toggled.connect(lambda m: self._on_track_mute(index, m))
        track.offset_changed.connect(lambda o: self._on_track_offset(index, o))
        track.splitter_moved.connect(self._on_splitter_moved)

        if index >= len(self._tracks):
            self._tracks.append(track)
            self.tracks_layout.addWidget(track)
        else:
            self._tracks[index] = track
            self.tracks_layout.insertWidget(index, track)

        track.set_playhead_position(self._playhead_position)
        track.set_alt_row(index % 2 == 0)
        # 延迟应用当前宽度（等待布局完成）
        QTimer.singleShot(0, lambda: track.set_controls_width(self._controls_width))
        self._update_max_height()
        self.updateGeometry()

        # 请求扩展高度
        track_count = len([t for t in self._tracks if t is not None])
        self.expand_requested.emit(track_count * self.TRACK_ROW_HEIGHT)

    def remove_track(self, index: int):
        """移除轨道"""
        if 0 <= index < len(self._tracks) and self._tracks[index] is not None:
            track = self._tracks.pop(index)
            self.tracks_layout.removeWidget(track)
            track.deleteLater()
            self._update_max_height()
            self.updateGeometry()  # 通知布局更新

    def update_playhead(self, position: float):
        """更新播放头位置 (0~1)"""
        self._playhead_position = position
        for track in self._tracks:
            if track is not None:
                track.set_playhead_position(position)

    def set_track_name(self, index: int, name: str):
        """设置轨道名称"""
        if 0 <= index < len(self._tracks) and self._tracks[index] is not None:
            self._tracks[index].set_file_name(name)

    def _on_track_remove(self, index: int):
        """轨道移除信号"""
        self.track_remove_clicked.emit(index)

    def _on_track_visibility(self, index: int, visible: bool):
        """轨道可见性信号"""
        self.track_visibility_toggled.emit(index, visible)

    def _on_track_mute(self, index: int, muted: bool):
        """轨道静音信号"""
        self.track_mute_toggled.emit(index, muted)

    def _on_track_offset(self, index: int, offset_ms: int):
        """轨道偏移信号"""
        self.track_offset_changed.emit(index, offset_ms)

    def _on_splitter_moved(self, new_width: int):
        """处理 splitter 拖动 - 同步所有轨道"""
        if self._syncing:
            return

        # 获取 sender（触发信号的 track）
        sender = self.sender()
        if sender is None:
            return

        # 限制最大比例
        total_width = sender.splitter.width()
        if total_width > 0:
            max_width = int(total_width * self.MAX_CONTROLS_RATIO)
            new_width = min(new_width, max_width)

        self._controls_width = new_width
        self._syncing = True

        # 先限制 sender 自身（会被 moveSplitter 限制在范围内）
        sender.set_controls_width(new_width)

        for track in self._tracks:
            if track is not None and track is not sender:
                track.set_controls_width(new_width)

        self._syncing = False

    def _sync_controls_width(self):
        """从现有 track 同步当前宽度（处理窗口 resize 的情况）"""
        for track in self._tracks:
            if track is not None:
                sizes = track.splitter.sizes()
                if sizes:
                    self._controls_width = sizes[0]
                break

    def _update_max_height(self):
        """更新最大高度限制 - 基于轨道数量"""
        track_count = len([t for t in self._tracks if t is not None])
        content_height = track_count * self.TRACK_ROW_HEIGHT
        # 设置为内容高度，窗体 40% 的限制由 MainWindow 的 splitter 控制
        self.setMaximumHeight(content_height if track_count > 0 else 0)

    def clear_tracks(self):
        """清除所有轨道"""
        for track in self._tracks:
            if track is not None:
                self.tracks_layout.removeWidget(track)
                track.deleteLater()
        self._tracks.clear()
        self._update_max_height()
