"""
ViewportPanel - 视频预览区域（支持并排/分屏模式）
重构版: 使用单一 MultiTrackGLWindow 替代多个 VideoPlaceholder
"""
from .gl_widget import ViewMode, MultiTrackGLWindow

from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QStackedWidget
from PySide6.QtCore import Signal, Qt
from qfluentwidgets_nuitka import FluentIcon, IconWidget, SubtitleLabel, IndeterminateProgressRing

from .header import MediaHeader
from ..theme_utils import get_color_hex, ColorKey


class ViewportPanel(QWidget):
    """
    视频预览区域 - 支持并排和分屏两种模式

    特性:
    - 使用单一 MultiTrackGLWindow 渲染所有视频 (通过 createWindowContainer 嵌入)
    - 切换模式时不销毁 OpenGL 实例
    - MediaHeader 行保持独立布局

    状态层 (使用 QStackedLayout 管理):
    - Loading: GL 初始化中，显示转圈圈
    - Empty: 无视频，显示空状态提示
    - Active: 显示 OpenGL 内容

    布局结构：
    ┌─────────────────────────────────┐
    │  GL Container (flex: 1)         │  <- createWindowContainer 包装的 GL 窗口
    ├─────────────────────────────────┤
    │  info_container (fixed)         │  <- MediaInfo 行
    │  [Info1]  [Info2]  [Info3]...   │
    └─────────────────────────────────┘
    """

    # 请求信号 (用户操作 → 请求外部处理)
    media_swap_requested = Signal(int, int)  # (slot_index, target_media_index) 请求交换
    media_settings_clicked = Signal(int)  # slot_index
    media_remove_clicked = Signal(int)  # slot_index

    # 状态信号
    split_position_changed = Signal(float)  # 分割线位置变化 (0.0 ~ 1.0)
    gl_initialized = Signal()  # OpenGL 上下文初始化完成

    def __init__(self, parent=None):
        super().__init__(parent)
        self._view_mode = ViewMode.SIDE_BY_SIDE
        self._sources: list[str] = []  # 缓存，用于显示
        self._info_items: list[MediaHeader] = []

        self._setup_ui()

    def _setup_ui(self):
        """设置布局"""
        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # === 内容区域：使用 QStackedWidget 管理三种状态 ===
        self._stacked_widget = QStackedWidget()

        # Page 0: 加载中提示 (Loading 状态)
        self._loading_placeholder = self._create_loading_placeholder()
        self._stacked_widget.addWidget(self._loading_placeholder)  # index 0

        # Page 1: 空状态占位提示 (Empty 状态)
        self._empty_placeholder = self._create_empty_placeholder()
        self._stacked_widget.addWidget(self._empty_placeholder)  # index 1

        # Page 2: GL Container (Active 状态)
        self._gl_window = MultiTrackGLWindow()
        self.gl_container = QWidget.createWindowContainer(self._gl_window, self)
        self.gl_container.setStyleSheet(f"background-color: {get_color_hex(ColorKey.BG_BASE)};")
        # 禁用容器接收拖放事件，让事件穿透到父窗口 (main_window)
        self.gl_container.setAcceptDrops(False)
        self._stacked_widget.addWidget(self.gl_container)  # index 2

        # 初始状态：Loading
        self._stacked_widget.setCurrentIndex(0)

        main_layout.addWidget(self._stacked_widget, 1)

        # === 第二行：MediaInfo 容器 ===
        self.info_container = QWidget(self)
        self.info_layout = QHBoxLayout(self.info_container)
        self.info_layout.setContentsMargins(0, 0, 0, 0)
        self.info_layout.setSpacing(0)
        main_layout.addWidget(self.info_container)

        # 连接 GL 窗口信号
        self._gl_window.split_position_changed.connect(self.split_position_changed.emit)
        self._gl_window.gl_initialized.connect(self._on_gl_initialized)

        # 初始状态：无视频时直接显示 empty_placeholder，不触发 GL 初始化
        # GL 初始化延迟到第一次有视频源时
        self._update_overlay_visibility()

    def _init_gl(self):
        """触发 GL 窗口初始化（通过显示 GL 容器）"""
        if self._gl_window.is_gl_initialized:
            return
        self._stacked_widget.setCurrentIndex(2)

    def _set_overlay_state(self, state: str):
        """设置覆盖层状态

        Args:
            state: "loading" | "empty" | "active"
        """
        if state == "loading":
            self._stacked_widget.setCurrentIndex(0)
        elif state == "empty":
            self._stacked_widget.setCurrentIndex(1)
        elif state == "active":
            self._stacked_widget.setCurrentIndex(2)

    def _on_gl_initialized(self):
        """GL 初始化完成回调"""
        # 根据当前 track 数量决定显示 Empty 还是 Active
        self._update_overlay_visibility()
        # 转发信号
        self.gl_initialized.emit()

    def _update_overlay_visibility(self):
        """更新覆盖层可见性（根据 GL 状态和 track 数量）

        优先级：empty > loading > active
        - 无视频时直接显示 empty_placeholder，不管 GL 是否初始化
        - 有视频但 GL 未初始化时显示 loading，并触发 GL 初始化
        - 有视频且 GL 已初始化时显示 active
        """
        if len(self._sources) == 0:
            # 无视频时直接显示 empty_placeholder
            self._set_overlay_state("empty")
        elif not self._gl_window.is_gl_initialized:
            # 有视频但 GL 未初始化，显示 loading 并触发初始化
            self._set_overlay_state("loading")
            self._init_gl()
        else:
            # 有视频且 GL 已初始化
            self._set_overlay_state("active")

        # 无视频时隐藏 info_container，避免留下空白条
        self.info_container.setVisible(len(self._sources) > 0)

    def _create_empty_placeholder(self) -> QWidget:
        """创建空状态占位提示"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        layout.setAlignment(Qt.AlignmentFlag.AlignCenter)

        icon = IconWidget(widget)
        icon.setIcon(FluentIcon.VIDEO)
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

    def _create_loading_placeholder(self) -> QWidget:
        """创建加载中占位提示"""
        widget = QWidget()
        layout = QVBoxLayout(widget)
        layout.setAlignment(Qt.AlignmentFlag.AlignCenter)

        progress_ring = IndeterminateProgressRing(widget)
        progress_ring.setFixedSize(48, 48)
        layout.addWidget(progress_ring, 0, Qt.AlignmentFlag.AlignCenter)

        layout.addSpacing(8)

        label = SubtitleLabel("正在初始化...")
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

    @property
    def gl_widget(self):
        """向后兼容属性，返回 GL 窗口实例"""
        return self._gl_window

    def _add_slot(self, index: int, current_source: str):
        """创建并添加槽位（info_item）"""
        info_item = MediaHeader(index, self._sources, current_source, self)
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
        self._clear_info_layout()
        for info in self._info_items:
            info.deleteLater()
        self._info_items.clear()

    def _clear_info_layout(self):
        """清空 info 布局"""
        while self.info_layout.count():
            item = self.info_layout.takeAt(0)
            if item.widget():
                item.widget().setParent(None)

    def _refresh_all_info_sources(self):
        """刷新所有 MediaHeader 的源列表"""
        for i, info in enumerate(self._info_items):
            info.update_sources(self._sources, self._sources[i] if i < len(self._sources) else "")

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

    # ========== 模式切换 ==========

    def set_view_mode(self, mode: ViewMode):
        """设置视图模式 (不销毁 GL 实例)"""
        if self._view_mode == mode:
            return

        self._view_mode = mode
        self._gl_window.set_view_mode(mode)
        self._update_info_layout()

    # ========== 公共 API (由 MainWindow 调用) ==========

    def set_sources(self, sources: list[str]):
        """设置所有媒体源 (全量更新)"""
        self._sources = sources.copy()
        self._clear_slots()

        for i, source in enumerate(sources):
            self._add_slot(i, source)

        # 更新 GL 窗口的 track 数量
        self._gl_window.set_track_count(len(sources))

        self._update_info_layout()
        self._update_overlay_visibility()

    def add_slot(self, source: str):
        """添加一个槽位"""
        self._sources.append(source)
        index = len(self._info_items)

        self._add_slot(index, source)

        # 更新所有 MediaHeader 的源列表
        self._refresh_all_info_sources()

        # 更新 GL 窗口
        self._gl_window.set_track_count(len(self._sources))

        self._update_info_layout()
        self._update_overlay_visibility()

    def remove_slot(self, index: int):
        """移除指定索引的槽位"""
        if not (0 <= index < len(self._sources)):
            return

        self._sources.pop(index)

        if index < len(self._info_items):
            info = self._info_items.pop(index)
            self._clear_info_layout()
            info.deleteLater()

        # 更新后续槽位的索引
        for i in range(index, len(self._info_items)):
            self._info_items[i].index = i

        # 更新所有 MediaHeader 的源列表
        self._refresh_all_info_sources()

        # 更新 GL 窗口
        self._gl_window.set_track_count(len(self._sources))

        self._update_info_layout()
        self._update_overlay_visibility()

    def on_sources_swapped(self, index1: int, index2: int):
        """响应源交换 (TrackManager.sources_swapped)"""
        if not (0 <= index1 < len(self._sources) and 0 <= index2 < len(self._sources)):
            return

        # 交换缓存中的源
        self._sources[index1], self._sources[index2] = \
            self._sources[index2], self._sources[index1]

        # 更新所有 MediaHeader 的显示
        self._refresh_all_info_sources()

        # 更新 GL 窗口的 track 顺序
        self._update_gl_track_order()

    def on_source_moved(self, old_index: int, new_index: int):
        """响应源移动 (TrackManager.sources_reordered)"""
        if not (0 <= old_index < len(self._sources) and 0 <= new_index < len(self._sources)):
            return

        # 移动缓存中的源
        source = self._sources.pop(old_index)
        self._sources.insert(new_index, source)

        # 重排 info_items
        info = self._info_items.pop(old_index)
        self._info_items.insert(new_index, info)

        # 更新所有槽位的索引
        for i, inf in enumerate(self._info_items):
            inf.index = i

        # 更新所有 MediaHeader 的源列表
        self._refresh_all_info_sources()

        # 更新 GL 窗口的 track 顺序
        self._update_gl_track_order()

        self._update_info_layout()

    def _update_gl_track_order(self):
        """更新 GL 窗口的 track 顺序"""
        order = list(range(len(self._sources)))
        self._gl_window.set_track_order(order)

    def set_decoders(self, decoders: list):
        """设置解码器列表 (供 GL 窗口使用)"""
        self._gl_window.set_decoders(decoders)

    @property
    def view_mode(self) -> ViewMode:
        return self._view_mode

    @property
    def split_position(self) -> float:
        return self._gl_window.split_position

    @split_position.setter
    def split_position(self, value: float):
        self._gl_window.set_split_position(value)

    @property
    def slot_count(self) -> int:
        """当前槽位数量"""
        return len(self._info_items)
