"""
TimelineArea - 时间轴轨道区域
"""
from typing import Optional
from PySide6.QtWidgets import QWidget, QVBoxLayout, QSizePolicy
from PySide6.QtCore import Signal
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

    def __init__(self, parent=None):
        super().__init__(parent)
        self._tracks: list[TrackRow] = []
        self._playhead_position = 0.0
        self._setup_ui()

    def _setup_ui(self):
        self.setStyleSheet(f"background-color: {get_color_hex(ColorKey.BG_TIMELINE)};")

        # 设置 sizePolicy 让高度根据内容自动调整
        self.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Preferred)

        self.main_layout = QVBoxLayout(self)
        self.main_layout.setContentsMargins(0, 0, 0, 0)
        self.main_layout.setSpacing(0)

    def add_track(self, index: int, name: str):
        """添加轨道"""
        # 确保 index 在有效范围内
        while len(self._tracks) < index:
            self._tracks.append(None)

        track = TrackRow(name, self)
        track.remove_clicked.connect(lambda: self._on_track_remove(index))
        track.visibility_toggled.connect(lambda v: self._on_track_visibility(index, v))
        track.mute_toggled.connect(lambda m: self._on_track_mute(index, m))
        track.offset_changed.connect(lambda o: self._on_track_offset(index, o))
        track.controls_width_changed.connect(self._on_controls_width_changed)

        if index >= len(self._tracks):
            self._tracks.append(track)
            self.main_layout.addWidget(track)
        else:
            self._tracks[index] = track
            self.main_layout.insertWidget(index, track)

        track.set_playhead_position(self._playhead_position)
        # 设置交替行样式：偶数索引稍亮，奇数索引保持基础色
        track.set_alt_row(index % 2 == 0)
        self.updateGeometry()  # 通知布局更新

    def remove_track(self, index: int):
        """移除轨道"""
        if 0 <= index < len(self._tracks) and self._tracks[index] is not None:
            track = self._tracks.pop(index)
            self.main_layout.removeWidget(track)
            track.deleteLater()
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

    def _on_controls_width_changed(self, new_width: int):
        """统一设置所有 TrackRow 的 controls_panel 宽度"""
        # 更新类级别的宽度
        TrackRow.set_controls_width(new_width)

        # 应用到所有轨道
        for track in self._tracks:
            if track is not None:
                track.apply_controls_width(new_width)

    def clear_tracks(self):
        """清除所有轨道"""
        for track in self._tracks:
            if track is not None:
                self.main_layout.removeWidget(track)
                track.deleteLater()
        self._tracks.clear()
