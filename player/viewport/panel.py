"""
ViewportPanel - 视频预览区域（支持并排/分屏模式）
"""
from enum import Enum

from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout
from PySide6.QtCore import Signal, Qt
from qfluentwidgets import isDarkTheme, FluentIcon, IconWidget, SubtitleLabel

from .placeholder import VideoPlaceholder
from .header import MediaHeader
from ..widgets.resizable_container import ResizableContainer
from ..theme_utils import get_color_hex, ColorKey


class ViewMode(Enum):
    """视图模式枚举"""
    SIDE_BY_SIDE = 0  # 并排模式 - 所有视频等分显示
    SPLIT_SCREEN = 1  # 分屏模式 - 只显示前两个，可拖动分割


class ViewportPanel(QWidget):
    """
    视频预览区域 - 支持并排和分屏两种模式

    数据流：
    - 用户操作 → 发射请求信号 (media_swap_requested, media_remove_clicked)
    - 外部通过 set_sources / add_slot / remove_slot / on_sources_swapped / on_source_moved 更新显示

    布局结构：
    ┌─────────────────────────────────┐
    │  panel_container (flex: 1)      │  <- 第一行：视频预览
    │  [Panel1] [Panel2] [Panel3]...  │
    ├─────────────────────────────────┤
    │  info_container (fixed 32px)    │  <- 第二行：媒体信息
    │  [Info1]  [Info2]  [Info3]...   │
    └─────────────────────────────────┘
    """

    # 请求信号 (用户操作 → 请求外部处理)
    media_swap_requested = Signal(int, int)  # (slot_index, target_media_index) 请求交换
    media_settings_clicked = Signal(int)  # slot_index
    media_remove_clicked = Signal(int)  # slot_index

    # 状态信号
    split_position_changed = Signal(float)  # 分割线位置变化 (0.0 ~ 1.0)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._view_mode = ViewMode.SIDE_BY_SIDE
        self._split_position = 0.5
        self._sources: list[str] = []  # 缓存，用于显示
        self._panels: list[VideoPlaceholder] = []
        self._info_items: list[MediaHeader] = []

        # 分屏模式下的容器
        self._container1: ResizableContainer | None = None
        self._container2: ResizableContainer | None = None
        self._syncing_width = False

        self._setup_ui()

    def _setup_ui(self):
        """设置布局"""
        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # 第一行：Panel 容器
        self.panel_container = QWidget(self)
        self.panel_layout = QHBoxLayout(self.panel_container)
        self.panel_layout.setContentsMargins(0, 0, 0, 0)
        self.panel_layout.setSpacing(0)
        main_layout.addWidget(self.panel_container, 1)

        # 空状态占位提示
        self._empty_placeholder = self._create_empty_placeholder()
        self.panel_layout.addWidget(self._empty_placeholder)

        # 第二行：MediaInfo 容器
        self.info_container = QWidget(self)
        self.info_layout = QHBoxLayout(self.info_container)
        self.info_layout.setContentsMargins(0, 0, 0, 0)
        self.info_layout.setSpacing(0)
        main_layout.addWidget(self.info_container)

    def _create_empty_placeholder(self) -> QWidget:
        """创建空状态占位提示"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        layout.setAlignment(Qt.AlignmentFlag.AlignCenter)

        icon = IconWidget(FluentIcon.VIDEO, widget)
        icon.setFixedSize(64, 64)
        layout.addWidget(icon, 0, Qt.AlignmentFlag.AlignCenter)

        layout.addSpacing(8)

        label = SubtitleLabel("点击「添加媒体」或拖放文件到这里")
        label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(label, 0, Qt.AlignmentFlag.AlignCenter)

        text_color = get_color_hex(ColorKey.TEXT_SECONDARY)
        bg_color = get_color_hex(ColorKey.BG_BASE)
        widget.setStyleSheet(f"""
            QWidget {{
                background-color: {bg_color};
            }}
            SubtitleLabel {{
                color: {text_color};
            }}
        """)

        return widget

    def _update_empty_placeholder(self):
        """更新空状态占位提示的可见性"""
        has_files = len(self._sources) > 0
        self._empty_placeholder.setVisible(not has_files)

    def _add_slot(self, index: int, current_source: str):
        """创建并添加槽位（panel + info_item）"""
        # 创建视频预览面板
        panel = VideoPlaceholder(index, self)
        self._panels.append(panel)

        # 创建媒体信息条
        info_item = MediaHeader(index, self._sources, current_source, self)
        # MediaHeader 选择改变时，请求交换
        info_item.media_changed.connect(
            lambda slot_idx, media_idx: self.media_swap_requested.emit(slot_idx, media_idx)
        )
        info_item.media_settings_clicked.connect(
            lambda slot_idx=index: self.media_settings_clicked.emit(slot_idx)
        )
        info_item.media_remove_clicked.connect(
            lambda slot_idx=index: self.media_remove_clicked.emit(slot_idx)
        )
        self._info_items.append(info_item)

    def _clear_slots(self):
        """清除所有槽位"""
        self._clear_panel_layout()
        self._clear_info_layout()
        self._clear_containers()

        for panel in self._panels:
            panel.deleteLater()
        for info in self._info_items:
            info.deleteLater()

        self._panels.clear()
        self._info_items.clear()

    def _clear_panel_layout(self):
        """清空 panel 布局（保留 _empty_placeholder）"""
        while self.panel_layout.count():
            item = self.panel_layout.takeAt(0)
            widget = item.widget()
            if widget and widget != self._empty_placeholder:
                widget.setParent(None)

    def _clear_info_layout(self):
        """清空 info 布局"""
        while self.info_layout.count():
            item = self.info_layout.takeAt(0)
            if item.widget():
                item.widget().setParent(None)

    def _clear_containers(self):
        """清除分屏模式的容器"""
        if self._container1:
            self._container1.takeWidget()
            self._container1.deleteLater()
            self._container1 = None
        if self._container2:
            self._container2.takeWidget()
            self._container2.deleteLater()
            self._container2 = None

    def _refresh_all_info_sources(self):
        """刷新所有 MediaHeader 的源列表"""
        for i, info in enumerate(self._info_items):
            info.update_sources(self._sources, self._sources[i] if i < len(self._sources) else "")

    # ========== 模式切换 ==========

    def set_view_mode(self, mode: ViewMode):
        """设置视图模式"""
        if self._view_mode == mode:
            return

        self._view_mode = mode

        if mode == ViewMode.SIDE_BY_SIDE:
            self._apply_side_by_side_mode()
        else:
            self._apply_split_screen_mode()

    def _apply_side_by_side_mode(self):
        """应用并排模式 - 所有面板固定 1/n 等分"""
        self._clear_panel_layout()
        self._clear_containers()

        for panel in self._panels:
            panel.setParent(None)
            self.panel_layout.addWidget(panel, 1)
            panel.show()

        self._update_info_layout()

    def _apply_split_screen_mode(self):
        """应用分屏模式 - 只显示前2个面板，双边可拖动"""
        if len(self._panels) < 2:
            return

        self._clear_panel_layout()
        self._clear_containers()

        panels_to_show = self._panels[:2]
        total_width = self.panel_container.width()
        if total_width < 200:
            total_width = 800

        half_width = total_width // 2

        self._container1 = ResizableContainer(self.panel_container)
        self._container1.setResizable(ResizableContainer.Edge.RIGHT)
        self._container1.setRange(0, total_width)
        self._container1.setCurrentWidth(half_width)
        self._container1.setFixedWidth(half_width)
        self._container1.setWidget(panels_to_show[0])
        self._container1.widthChanged.connect(self._on_container1_width_changed)
        self.panel_layout.addWidget(self._container1)

        self._container2 = ResizableContainer(self.panel_container)
        self._container2.setResizable(ResizableContainer.Edge.LEFT)
        self._container2.setRange(0, total_width)
        self._container2.setCurrentWidth(half_width)
        self._container2.setFixedWidth(half_width)
        self._container2.setWidget(panels_to_show[1])
        self._container2.widthChanged.connect(self._on_container2_width_changed)
        self.panel_layout.addWidget(self._container2)

        for panel in self._panels[2:]:
            panel.hide()

        self._update_info_layout()

    def _update_info_layout(self):
        """更新 MediaInfo 行布局"""
        self._clear_info_layout()

        visible_count = len(self._info_items)
        if self._view_mode == ViewMode.SPLIT_SCREEN:
            visible_count = min(2, len(self._info_items))

        for i, info in enumerate(self._info_items):
            info.setParent(None)
            if i < visible_count:
                self.info_layout.addWidget(info, 1)
                info.show()
            else:
                info.hide()

    def _on_container1_width_changed(self, width: int):
        """第一个 container 宽度变化时联动第二个"""
        if self._syncing_width or not self._container2:
            return
        self._syncing_width = True

        total_width = self.panel_container.width()
        remaining = total_width - width

        self._container1.setFixedWidth(width)
        self._container2.setFixedWidth(remaining)
        self._container2.setCurrentWidth(remaining)

        self._container1.setRange(0, total_width)
        self._container2.setRange(0, total_width)

        if total_width > 0:
            self._split_position = width / total_width
            self.split_position_changed.emit(self._split_position)

        self._syncing_width = False

    def _on_container2_width_changed(self, width: int):
        """第二个 container 宽度变化时联动第一个"""
        if self._syncing_width or not self._container1:
            return
        self._syncing_width = True

        total_width = self.panel_container.width()
        new_width1 = total_width - width

        self._container1.setFixedWidth(new_width1)
        self._container1.setCurrentWidth(new_width1)
        self._container2.setFixedWidth(width)

        self._container1.setRange(0, total_width)
        self._container2.setRange(0, total_width)

        if total_width > 0:
            self._split_position = new_width1 / total_width
            self.split_position_changed.emit(self._split_position)

        self._syncing_width = False

    # ========== 公共 API (由 MainWindow 调用) ==========

    def set_sources(self, sources: list[str]):
        """设置所有媒体源 (全量更新)"""
        self._sources = sources.copy()
        self._clear_slots()

        for i, source in enumerate(sources):
            self._add_slot(i, source)

        if self._view_mode == ViewMode.SIDE_BY_SIDE:
            self._apply_side_by_side_mode()
        else:
            self._apply_split_screen_mode()

        self._update_empty_placeholder()

    def add_slot(self, source: str):
        """添加一个槽位"""
        self._sources.append(source)
        index = len(self._panels)

        self._add_slot(index, source)

        # 更新所有 MediaHeader 的源列表
        self._refresh_all_info_sources()

        # 重新应用当前模式
        if self._view_mode == ViewMode.SIDE_BY_SIDE:
            self._apply_side_by_side_mode()
        else:
            self._apply_split_screen_mode()

        self._update_empty_placeholder()

    def remove_slot(self, index: int):
        """移除指定索引的槽位"""
        if not (0 <= index < len(self._sources)):
            return

        self._sources.pop(index)

        if index < len(self._panels):
            panel = self._panels.pop(index)
            info = self._info_items.pop(index)
            self._clear_panel_layout()
            self._clear_info_layout()
            panel.deleteLater()
            info.deleteLater()

        # 更新后续槽位的索引
        for i in range(index, len(self._panels)):
            self._panels[i].index = i
            self._info_items[i].index = i

        # 更新所有 MediaHeader 的源列表
        self._refresh_all_info_sources()

        # 重新应用当前模式
        if self._view_mode == ViewMode.SIDE_BY_SIDE:
            self._apply_side_by_side_mode()
        else:
            self._apply_split_screen_mode()

        self._update_empty_placeholder()

    def on_sources_swapped(self, index1: int, index2: int):
        """响应源交换 (TrackManager.sources_swapped)"""
        if not (0 <= index1 < len(self._sources) and 0 <= index2 < len(self._sources)):
            return

        # 交换缓存中的源
        self._sources[index1], self._sources[index2] = \
            self._sources[index2], self._sources[index1]

        # 更新所有 MediaHeader 的显示
        self._refresh_all_info_sources()

    def on_source_moved(self, old_index: int, new_index: int):
        """响应源移动 (TrackManager.sources_reordered)"""
        if not (0 <= old_index < len(self._sources) and 0 <= new_index < len(self._sources)):
            return

        # 移动缓存中的源
        source = self._sources.pop(old_index)
        self._sources.insert(new_index, source)

        # 重排 panels
        panel = self._panels.pop(old_index)
        self._panels.insert(new_index, panel)

        # 重排 info_items
        info = self._info_items.pop(old_index)
        self._info_items.insert(new_index, info)

        # 更新所有槽位的索引
        for i, (p, inf) in enumerate(zip(self._panels, self._info_items)):
            p.index = i
            inf.index = i

        # 更新所有 MediaHeader 的源列表
        self._refresh_all_info_sources()

        # 重新应用当前模式
        if self._view_mode == ViewMode.SIDE_BY_SIDE:
            self._apply_side_by_side_mode()
        else:
            self._apply_split_screen_mode()

    @property
    def view_mode(self) -> ViewMode:
        return self._view_mode

    @property
    def split_position(self) -> float:
        return self._split_position

    @split_position.setter
    def split_position(self, value: float):
        self._split_position = max(0.0, min(1.0, value))
        if self._container1 and self._container2:
            total_width = self.panel_container.width()
            left_width = int(total_width * self._split_position)
            right_width = total_width - left_width
            self._container1.setFixedWidth(left_width)
            self._container1.setCurrentWidth(left_width)
            self._container2.setFixedWidth(right_width)
            self._container2.setCurrentWidth(right_width)

    @property
    def slot_count(self) -> int:
        """当前槽位数量"""
        return len(self._panels)
