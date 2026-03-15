"""
ViewportPanel - 视频预览区域（支持并排/分屏模式）
"""
from enum import Enum

from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout
from PySide6.QtCore import Signal

from .placeholder import VideoPlaceholder
from .header import MediaHeader
from ..widgets.resizable_container import ResizableContainer


class ViewMode(Enum):
    """视图模式枚举"""
    SIDE_BY_SIDE = 0  # 并排模式 - 所有视频等分显示
    SPLIT_SCREEN = 1  # 分屏模式 - 只显示前两个，可拖动分割


class ViewportPanel(QWidget):
    """
    视频预览区域 - 支持并排和分屏两种模式

    布局结构：
    ┌─────────────────────────────────┐
    │  panel_container (flex: 1)      │  <- 第一行：视频预览
    │  [Panel1] [Panel2] [Panel3]...  │
    ├─────────────────────────────────┤
    │  info_container (fixed 32px)    │  <- 第二行：媒体信息
    │  [Info1]  [Info2]  [Info3]...   │
    └─────────────────────────────────┘

    并排模式(SIDE_BY_SIDE)：
    - 显示所有视频面板和信息条
    - 固定 1/n 等分，不可调整宽度

    分屏模式(SPLIT_SCREEN)：
    - 只显示前两个视频面板和信息条
    - 使用 ResizableContainer，双边可拖动调整
    """

    # 信号
    split_position_changed = Signal(float)  # 分割线位置变化 (0.0 ~ 1.0)
    media_changed = Signal(int, int)  # (slot_index, selected_media_index)
    media_settings_clicked = Signal(int)  # slot_index
    media_remove_clicked = Signal(int)  # slot_index

    def __init__(self, parent=None):
        super().__init__(parent)
        self._view_mode = ViewMode.SIDE_BY_SIDE
        self._split_position = 0.5
        self._sources: list[str] = []
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

        # 第二行：MediaInfo 容器
        self.info_container = QWidget(self)
        self.info_container.setFixedHeight(32)
        self.info_layout = QHBoxLayout(self.info_container)
        self.info_layout.setContentsMargins(8, 4, 8, 0)
        self.info_layout.setSpacing(0)
        main_layout.addWidget(self.info_container)

    def _add_slot(self, index: int, current_source: str):
        """创建并添加槽位（panel + info_item）"""
        # 创建视频预览面板
        panel = VideoPlaceholder(index, self)
        self._panels.append(panel)

        # 创建媒体信息条
        info_item = MediaHeader(index, self._sources, current_source, self)
        info_item.media_changed.connect(
            lambda media_idx, slot_idx=index: self.media_changed.emit(slot_idx, media_idx)
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
        """清空 panel 布局"""
        while self.panel_layout.count():
            item = self.panel_layout.takeAt(0)
            if item.widget():
                item.widget().setParent(None)

    def _clear_info_layout(self):
        """清空 info 布局"""
        while self.info_layout.count():
            item = self.info_layout.takeAt(0)
            if item.widget():
                item.widget().setParent(None)

    def _clear_containers(self):
        """清除分屏模式的容器"""
        if self._container1:
            self._container1.takeWidget()  # 清理事件过滤器和光标
            self._container1.deleteLater()
            self._container1 = None
        if self._container2:
            self._container2.takeWidget()  # 清理事件过滤器和光标
            self._container2.deleteLater()
            self._container2 = None

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

        # 所有 panel 直接添加，等分显示
        for panel in self._panels:
            panel.setParent(None)
            self.panel_layout.addWidget(panel, 1)  # stretch=1 等分
            panel.show()

        # 所有 info 等分显示
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
            total_width = 800  # 默认值

        half_width = total_width // 2

        # 第一个 container：启用 RIGHT 边缘拖动，使用固定宽度
        self._container1 = ResizableContainer(self.panel_container)
        self._container1.setResizable(ResizableContainer.Edge.RIGHT)
        self._container1.setRange(0, total_width)  # 允许压缩到 0
        self._container1.setCurrentWidth(half_width)
        self._container1.setFixedWidth(half_width)
        self._container1.setWidget(panels_to_show[0])
        self._container1.widthChanged.connect(self._on_container1_width_changed)
        self.panel_layout.addWidget(self._container1)

        # 第二个 container：启用 LEFT 边缘拖动，也使用固定宽度
        self._container2 = ResizableContainer(self.panel_container)
        self._container2.setResizable(ResizableContainer.Edge.LEFT)
        self._container2.setRange(0, total_width)  # 允许压缩到 0
        self._container2.setCurrentWidth(half_width)
        self._container2.setFixedWidth(half_width)
        self._container2.setWidget(panels_to_show[1])
        self._container2.widthChanged.connect(self._on_container2_width_changed)
        self.panel_layout.addWidget(self._container2)

        # 隐藏第 3 个及之后的 panel
        for panel in self._panels[2:]:
            panel.hide()

        # 更新 info 布局（只显示前 2 个）
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
                self.info_layout.addWidget(info, 1)  # stretch=1 等分
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

        # 同时更新两个 container 的固定宽度
        self._container1.setFixedWidth(width)
        self._container2.setFixedWidth(remaining)
        self._container2.setCurrentWidth(remaining)

        # 更新 range
        self._container1.setRange(0, total_width)
        self._container2.setRange(0, total_width)

        # 更新分割位置
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

        # 同时更新两个 container 的固定宽度
        self._container1.setFixedWidth(new_width1)
        self._container1.setCurrentWidth(new_width1)
        self._container2.setFixedWidth(width)

        # 更新 range
        self._container1.setRange(0, total_width)
        self._container2.setRange(0, total_width)

        # 更新分割位置
        if total_width > 0:
            self._split_position = new_width1 / total_width
            self.split_position_changed.emit(self._split_position)

        self._syncing_width = False

    # ========== 公共 API ==========

    def set_sources(self, sources: list[str]):
        """设置所有可用的媒体源列表"""
        self._sources = sources

        # 清除现有槽位
        self._clear_slots()

        # 创建新的槽位
        for i, source in enumerate(sources):
            self._add_slot(i, source)

        # 应用当前模式
        if self._view_mode == ViewMode.SIDE_BY_SIDE:
            self._apply_side_by_side_mode()
        else:
            self._apply_split_screen_mode()

    def add_source(self, source: str):
        """添加一个新的媒体源"""
        self._sources.append(source)
        index = len(self._panels)

        # 创建新槽位
        self._add_slot(index, source)

        # 更新所有槽位的源列表
        for info in self._info_items:
            info.update_sources(self._sources)

        # 重新应用当前模式
        if self._view_mode == ViewMode.SIDE_BY_SIDE:
            self._apply_side_by_side_mode()
        else:
            self._apply_split_screen_mode()

    def remove_source(self, index: int):
        """移除指定索引的媒体源"""
        if 0 <= index < len(self._sources):
            self._sources.pop(index)

            # 移除对应的槽位
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

            # 更新所有槽位的源列表
            for info in self._info_items:
                info.update_sources(self._sources)

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
        # 更新容器宽度
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
