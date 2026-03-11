"""
MediaInfoBar - 媒体名称显示条
"""
from typing import Optional
from PySide6.QtWidgets import QWidget, QHBoxLayout
from PySide6.QtCore import Signal
from qfluentwidgets import (
    ComboBox,
    FluentIcon,
)

from .widgets import create_tool_button


class MediaInfoItem(QWidget):
    """单个媒体信息项"""

    # 信号
    media_changed = Signal(int, int)  # (item_index, selected_media_index)
    media_settings_clicked = Signal(int)  # index
    media_remove_clicked = Signal(int)  # index

    def __init__(self, index: int, sources: list[str], current_source: str = "", parent=None):
        super().__init__(parent)
        self.index = index
        self._sources = sources
        self._current_source = current_source
        self.setFixedHeight(32)
        self._setup_ui()

    def _setup_ui(self):
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 8, 0)  # 左边距由 MediaInfoBar 统一控制，右边距给按钮留空间
        layout.setSpacing(8)

        # 媒体选择下拉框
        self.media_combo = ComboBox(self)
        self.media_combo.setPlaceholderText("选择媒体")
        self._update_combo_items()
        self.media_combo.currentIndexChanged.connect(self._on_selection_changed)
        layout.addWidget(self.media_combo)

        layout.addStretch()

        # 设置按钮
        self.settings_btn = create_tool_button(FluentIcon.SETTING, self, 24)
        self.settings_btn.setToolTip("媒体设置")
        self.settings_btn.clicked.connect(lambda: self.media_settings_clicked.emit(self.index))
        layout.addWidget(self.settings_btn)

        # 关闭按钮
        self.remove_btn = create_tool_button(FluentIcon.CLOSE, self, 24)
        self.remove_btn.setToolTip("移除媒体")
        self.remove_btn.clicked.connect(lambda: self.media_remove_clicked.emit(self.index))
        layout.addWidget(self.remove_btn)

    def _update_combo_items(self):
        """更新下拉框选项"""
        self.media_combo.blockSignals(True)
        self.media_combo.clear()

        for source in self._sources:
            self.media_combo.addItem(source)

        # 设置当前选中项
        if self._current_source and self._current_source in self._sources:
            self.media_combo.setCurrentIndex(self._sources.index(self._current_source))

        self.media_combo.blockSignals(False)

    def _on_selection_changed(self, combo_index: int):
        """下拉框选择改变"""
        if 0 <= combo_index < len(self._sources):
            self._current_source = self._sources[combo_index]
            self.media_changed.emit(self.index, combo_index)

    def update_sources(self, sources: list[str], current_source: str = None):
        """更新可选媒体源列表"""
        self._sources = sources
        if current_source is not None:
            self._current_source = current_source
        self._update_combo_items()

    def set_current_source(self, source: str):
        """设置当前选中的媒体源"""
        self._current_source = source
        self._update_combo_items()


class MediaInfoBar(QWidget):
    """媒体名称显示条 - 显示每个视频的媒体信息"""

    # 信号
    media_changed = Signal(int, int)  # (item_index, selected_media_index)
    media_settings_clicked = Signal(int)  # index
    media_remove_clicked = Signal(int)  # index

    def __init__(self, parent=None):
        super().__init__(parent)
        self._media_items: list[MediaInfoItem] = []
        self._sources: list[str] = []  # 所有可用的媒体源
        self.setFixedHeight(36)  # 32px 内容 + 4px 顶部边距
        self._setup_ui()

    def _setup_ui(self):
        self.main_layout = QHBoxLayout(self)
        self.main_layout.setContentsMargins(8, 4, 8, 0)
        self.main_layout.setSpacing(0)

    def set_sources(self, sources: list[str]):
        """设置所有可用的媒体源列表"""
        self._sources = sources
        # 更新所有 MediaInfoItem 的下拉框选项
        for item in self._media_items:
            item.update_sources(self._sources)

    def add_source(self, source: str):
        """添加一个新的媒体源"""
        self._sources.append(source)
        # 更新所有 MediaInfoItem 的下拉框选项
        for item in self._media_items:
            item.update_sources(self._sources)

    def remove_source(self, source: str):
        """移除一个媒体源"""
        if source in self._sources:
            self._sources.remove(source)
            # 更新所有 MediaInfoItem 的下拉框选项
            for item in self._media_items:
                item.update_sources(self._sources)

    def set_media_count(self, count: int):
        """设置媒体项数量"""
        # 清除现有项
        for item in self._media_items:
            item.deleteLater()
        self._media_items.clear()

        # 创建新的媒体项
        for i in range(count):
            self._add_media_item(i, "")

    def add_media_item(self, current_source: str = ""):
        """添加一个新的媒体项（增量添加）"""
        index = len(self._media_items)
        self._add_media_item(index, current_source)

    def _add_media_item(self, index: int, current_source: str):
        """内部方法：创建并添加媒体项"""
        item = MediaInfoItem(index, self._sources, current_source, self)
        item.media_changed.connect(self.media_changed.emit)
        item.media_settings_clicked.connect(self.media_settings_clicked.emit)
        item.media_remove_clicked.connect(self.media_remove_clicked.emit)
        self.main_layout.addWidget(item)
        self._media_items.append(item)

    def remove_media_item(self, index: int):
        """移除指定索引的媒体项，并更新后续项的索引"""
        if 0 <= index < len(self._media_items):
            item = self._media_items.pop(index)
            item.deleteLater()
            # 更新后续项的索引
            for i in range(index, len(self._media_items)):
                self._media_items[i].index = i

    def set_media_name(self, index: int, name: str):
        """设置指定索引的媒体项当前选中的源"""
        if 0 <= index < len(self._media_items):
            self._media_items[index].set_current_source(name)
