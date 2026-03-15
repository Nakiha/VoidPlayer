"""
MediaHeader - 媒体信息头部控件
"""
from PySide6.QtWidgets import QWidget, QHBoxLayout, QSizePolicy
from PySide6.QtCore import Signal
from qfluentwidgets import (
    ComboBox,
    FluentIcon,
)

from ..widgets import create_tool_button


class MediaHeader(QWidget):
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
        self._setup_ui()

    def _setup_ui(self):
        # 设置尺寸策略：水平方向扩展填充
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        # 设置最小宽度，防止被过度压缩
        self.setMinimumWidth(100)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)  # 自己控制边距，不受容器影响
        layout.setSpacing(4)

        # 媒体选择下拉框
        self.media_combo = ComboBox(self)
        self.media_combo.setPlaceholderText("选择媒体")
        # 使用 Minimum 策略：允许压缩但不小于 minimumSizeHint
        self.media_combo.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Fixed)
        self.media_combo.setMinimumWidth(50)
        self._update_combo_items()
        self.media_combo.currentIndexChanged.connect(self._on_selection_changed)
        layout.addWidget(self.media_combo, 1)  # stretch=1 让 ComboBox 占据可用空间

        layout.addStretch()

        # 设置按钮
        self.settings_btn = create_tool_button(FluentIcon.SETTING, self, 28, "媒体设置")
        self.settings_btn.clicked.connect(lambda: self.media_settings_clicked.emit(self.index))
        layout.addWidget(self.settings_btn)

        # 关闭按钮
        self.remove_btn = create_tool_button(FluentIcon.CLOSE, self, 28, "移除媒体")
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
