"""
MediaHeader - 媒体信息头部控件
"""
from PySide6.QtWidgets import QWidget, QHBoxLayout, QVBoxLayout, QSizePolicy
from PySide6.QtCore import Signal, Qt
from qfluentwidgets_nuitka import FluentIcon

from ..widgets import create_tool_button, ElideComboBox


class MediaHeader(QWidget):
    """单个媒体信息项"""

    # 信号
    media_changed = Signal(int, int)  # (item_index, selected_media_index)
    media_settings_clicked = Signal(int)  # index
    media_remove_clicked = Signal(int)  # index

    # 按钮区域所需的最小宽度
    BUTTONS_MIN_WIDTH = 70  # 28 + 4 + 28 + 10(margin)

    def __init__(self, index: int, sources: list[str], current_source: str = "", parent=None):
        super().__init__(parent)
        self.index = index
        self._sources = sources
        self._current_source = current_source
        self._two_line_mode = False
        self._setup_ui()

    def _setup_ui(self):
        # 设置尺寸策略：水平方向扩展填充
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        # 设置最小宽度，防止被过度压缩
        self.setMinimumWidth(100)

        # 主布局
        self.main_layout = QVBoxLayout(self)
        self.main_layout.setContentsMargins(4, 4, 4, 4)
        self.main_layout.setSpacing(0)

        # 第一行布局
        self.row1 = QWidget(self)
        self.row1_layout = QHBoxLayout(self.row1)
        self.row1_layout.setContentsMargins(0, 0, 0, 0)
        self.row1_layout.setSpacing(4)
        self.main_layout.addWidget(self.row1)

        # 第二行布局（初始隐藏）
        self.row2 = QWidget(self)
        self.row2_layout = QHBoxLayout(self.row2)
        self.row2_layout.setContentsMargins(0, 4, 0, 0)  # 顶部间距
        self.row2_layout.setSpacing(4)
        self.row2.hide()
        self.main_layout.addWidget(self.row2)

        # 媒体选择下拉框（支持文本省略）
        self.media_combo = ElideComboBox(self.row1)
        self.media_combo.setPlaceholderText("选择媒体")
        # 使用 Minimum 策略：允许压缩但不小于 minimumSizeHint
        self.media_combo.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Fixed)
        self.media_combo.setMinimumWidth(50)
        self.media_combo.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # 不拦截快捷键
        self._update_combo_items()
        self.media_combo.currentIndexChanged.connect(self._on_selection_changed)
        self.row1_layout.addWidget(self.media_combo, 1)  # stretch=1 让 ComboBox 占据可用空间

        # 按钮容器（放在第一行）
        self.buttons_widget = QWidget(self.row1)
        self.buttons_layout = QHBoxLayout(self.buttons_widget)
        self.buttons_layout.setContentsMargins(0, 0, 0, 0)
        self.buttons_layout.setSpacing(4)
        self.row1_layout.addWidget(self.buttons_widget)

        # 设置按钮
        self.settings_btn = create_tool_button(FluentIcon.SETTING, self.buttons_widget, 28, "媒体设置")
        self.settings_btn.clicked.connect(lambda: self.media_settings_clicked.emit(self.index))
        self.buttons_layout.addWidget(self.settings_btn)

        # 关闭按钮
        self.remove_btn = create_tool_button(FluentIcon.CLOSE, self.buttons_widget, 28, "移除媒体")
        self.remove_btn.clicked.connect(lambda: self.media_remove_clicked.emit(self.index))
        self.buttons_layout.addWidget(self.remove_btn)

    def resizeEvent(self, event):
        """调整大小时检查是否需要切换到两行模式"""
        super().resizeEvent(event)
        self._check_layout_mode()

    def _check_layout_mode(self):
        """检查并切换布局模式"""
        # 计算可用宽度
        available_width = self.width() - 8  # 减去左右边距

        # 计算 ComboBox 的理想宽度
        combo_min = self.media_combo.minimumWidth()
        buttons_width = self.BUTTONS_MIN_WIDTH

        # 如果空间不够，切换到两行模式
        need_two_line = (combo_min + buttons_width + 8) > available_width

        if need_two_line != self._two_line_mode:
            self._two_line_mode = need_two_line
            self._apply_layout_mode()

    def _apply_layout_mode(self):
        """应用布局模式"""
        if self._two_line_mode:
            # 将按钮移到第二行
            self.buttons_widget.setParent(self.row2)
            self.row2_layout.addWidget(self.buttons_widget)
            self.row2_layout.addStretch()
            self.row2.show()
        else:
            # 将按钮移回第一行
            self.buttons_widget.setParent(self.row1)
            self.row1_layout.addWidget(self.buttons_widget)
            self.row2.hide()

    def _update_combo_items(self):
        """更新下拉框选项"""
        self.media_combo.blockSignals(True)
        self.media_combo.clear()

        for source in self._sources:
            self.media_combo.addItem(source)  # 传完整路径，ElideComboBox 自动处理显示

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
