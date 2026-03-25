"""
TimelineArea - 时间轴轨道区域
"""
from typing import Optional, TYPE_CHECKING
from PySide6.QtWidgets import QWidget, QVBoxLayout, QSizePolicy, QFrame
from PySide6.QtCore import Signal, QTimer, Qt
from qfluentwidgets_nuitka import SmoothScrollArea

from .track_row import TrackRow
from .theme_utils import get_color_hex, get_accent_color_hex, ColorKey
from ..core.signal_bus import signal_bus

if TYPE_CHECKING:
    from ..core.decoder_pool import MediaInfo


class TimelineArea(QWidget):
    """时间轴轨道区域 - 管理多条轨道"""

    # 请求信号 (用户操作 → 请求外部处理)
    track_remove_clicked = Signal(int)  # index
    track_move_requested = Signal(int, int)  # old_index, new_index (请求移动)

    # 转发信号
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
        self._drag_index = -1  # 当前拖拽的 track 索引
        self._drop_index = -1  # 当前悬停的目标位置
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

        # 插入指示线 (初始隐藏)
        self._drop_indicator = QFrame(self.scroll_content)
        self._drop_indicator.setFixedHeight(3)
        self._drop_indicator.setStyleSheet(f"background-color: {get_accent_color_hex()}; border-radius: 1px;")
        self._drop_indicator.hide()

        # 更新最大高度
        self._update_max_height()

    def add_track(self, index: int, name: str, media_info: Optional["MediaInfo"] = None):
        """添加轨道

        Args:
            index: 轨道索引
            name: 文件名/路径
            media_info: 可选的媒体信息 (包含时长等)
        """
        # 确保 index 在有效范围内
        while len(self._tracks) < index:
            self._tracks.append(None)

        # 从现有 track 获取当前实际宽度（解决窗口 resize 后宽度不同步的问题）
        self._sync_controls_width()

        track = TrackRow(name, self)
        # 所有信号都使用动态索引（重排序后索引会变化）
        track.remove_clicked.connect(lambda: self._on_track_remove(self._tracks.index(track)))
        track.visibility_toggled.connect(lambda v: self._on_track_visibility(self._tracks.index(track), v))
        track.mute_toggled.connect(lambda m: self._on_track_mute(self._tracks.index(track), m))
        track.offset_changed.connect(lambda o: self._on_track_offset(self._tracks.index(track), o))
        track.splitter_moved.connect(self._on_splitter_moved)
        track.drag_started.connect(lambda: self._on_track_drag_started(self._tracks.index(track)))
        track.drag_moved.connect(lambda y: self._on_track_drag_moved(self._tracks.index(track), y))
        track.drag_finished.connect(lambda: self._on_track_drag_finished(self._tracks.index(track)))

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
        signal_bus.media_remove_requested.emit(index)

    def _on_track_visibility(self, index: int, visible: bool):
        """轨道可见性信号"""
        signal_bus.track_visibility_changed.emit(index, visible)

    def _on_track_mute(self, index: int, muted: bool):
        """轨道静音信号"""
        signal_bus.track_mute_changed.emit(index, muted)

    def _on_track_offset(self, index: int, offset_ms: int):
        """轨道偏移信号"""
        signal_bus.sync_offset_changed.emit(index, offset_ms)

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

    # ========== 拖拽重排序 ==========

    def _on_track_drag_started(self, index: int):
        """轨道拖拽开始"""
        if len(self._tracks) < 2:
            return  # 单条轨道不允许拖拽

        self._drag_index = index
        self._drop_index = index

        # 设置拖拽样式
        if self._tracks[index] is not None:
            self._tracks[index].set_dragging(True)

    def _on_track_drag_moved(self, track_index: int, global_y: int):
        """轨道拖拽移动"""
        if self._drag_index < 0:
            return

        # 计算目标索引
        drop_index = self._calculate_drop_index(global_y)

        if drop_index != self._drop_index:
            self._drop_index = drop_index
            self._update_drop_indicator()

    def _on_track_drag_finished(self, track_index: int):
        """轨道拖拽结束"""
        if self._drag_index < 0:
            return

        # 隐藏指示线
        self._drop_indicator.hide()

        # 恢复拖拽样式
        if self._drag_index < len(self._tracks) and self._tracks[self._drag_index] is not None:
            self._tracks[self._drag_index].set_dragging(False)

        # 执行重排序
        old_index = self._drag_index
        new_index = self._drop_index

        # 重置状态
        self._drag_index = -1
        self._drop_index = -1

        # 只有位置变化才触发重排序请求
        if old_index != new_index:
            signal_bus.track_move_requested.emit(old_index, new_index)

    def _calculate_drop_index(self, global_y: int) -> int:
        """根据全局 Y 坐标计算目标索引"""
        # 将全局坐标转换为滚动内容区域的坐标
        local_y = self.scroll_content.mapFromGlobal(self.scroll_content.mapToGlobal(
            self.scroll_content.pos()
        )).y() + global_y - self.mapToGlobal(self.pos()).y()

        # 直接使用 global_y 相对于 scroll_content 的位置
        scroll_content_global = self.scroll_content.mapToGlobal(self.scroll_content.pos())
        local_y = global_y - scroll_content_global.y() + self.scroll_content.y()

        # 简化计算：根据 y 位置计算索引
        track_count = len([t for t in self._tracks if t is not None])
        if track_count == 0:
            return 0

        # 计算目标索引
        index = local_y // self.TRACK_ROW_HEIGHT
        index = max(0, min(index, track_count))

        # 如果拖到自己的位置，返回原索引
        if index == self._drag_index:
            return self._drag_index

        # 如果拖到被拖拽项之后的位置，需要调整
        if index > self._drag_index:
            index = min(index, track_count - 1)

        return index

    def _update_drop_indicator(self):
        """更新插入指示线位置"""
        if self._drop_index < 0:
            self._drop_indicator.hide()
            return

        # 计算指示线 Y 位置
        y = self._drop_index * self.TRACK_ROW_HEIGHT

        # 如果拖到被拖拽项之后，位置需要调整
        if self._drop_index > self._drag_index:
            y = (self._drop_index + 1) * self.TRACK_ROW_HEIGHT

        # 设置指示线位置和大小
        self._drop_indicator.setGeometry(0, y - 1, self.scroll_content.width(), 3)
        self._drop_indicator.raise_()
        self._drop_indicator.show()

    def reorder_track(self, old_index: int, new_index: int):
        """重排序轨道 (由 MainWindow 在 TrackManager 信号后调用)"""
        if old_index == new_index:
            return

        if not (0 <= old_index < len(self._tracks)):
            return

        if not (0 <= new_index < len(self._tracks)):
            return

        track = self._tracks.pop(old_index)
        self._tracks.insert(new_index, track)

        # 更新布局
        self.tracks_layout.removeWidget(track)
        self.tracks_layout.insertWidget(new_index, track)

        # 更新交替行样式
        for i, t in enumerate(self._tracks):
            if t is not None:
                t.set_alt_row(i % 2 == 0)
