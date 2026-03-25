"""
ToolBar - 顶部工具栏
"""
from PySide6.QtWidgets import QWidget, QHBoxLayout, QSpacerItem, QSizePolicy, QGraphicsOpacityEffect
from PySide6.QtCore import Signal, Qt
from qfluentwidgets_nuitka import (
    PushButton,
    PrimaryPushButton,
    TransparentToolButton,
    SegmentedWidget,
    FluentIcon,
)

from player.core.signal_bus import signal_bus

from .viewport import ViewMode
from .widgets import create_tool_button


class ToolBar(QWidget):
    """顶部工具栏 - 视图模式切换和项目操作

    通过 signal_bus 发送请求信号。
    """

    # 内部信号 (用于 MainWindow 的视图模式同步)
    view_mode_changed = Signal(ViewMode)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedHeight(40)  # 32px 内容 + 4px * 2 边距
        self._setup_ui()
        self.set_view_mode_enabled(False)  # 初始禁用（无文件）

    def _setup_ui(self):
        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(0)

        # 左侧: 视图模式切换
        self.view_mode_segment = SegmentedWidget(self)
        self.view_mode_segment.addItem("side_by_side", "并排", lambda: self._on_view_mode_changed(ViewMode.SIDE_BY_SIDE))
        self.view_mode_segment.addItem("split_screen", "分屏", lambda: self._on_view_mode_changed(ViewMode.SPLIT_SCREEN))
        self.view_mode_segment.setCurrentItem("side_by_side")
        self.view_mode_segment.setFixedSize(240, 32)  # 固定宽度，与添加媒体按钮同高

        # 设置按钮铺满 SegmentedWidget
        for item in self.view_mode_segment.items.values():
            item.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Expanding)

        layout.addWidget(self.view_mode_segment)

        # 弹性空间
        layout.addSpacerItem(QSpacerItem(40, 20, QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum))

        # 右侧: 操作按钮
        # 添加媒体 (绿色主按钮)
        self.add_media_btn = PrimaryPushButton("添加媒体", self)
        self.add_media_btn.setFixedHeight(32)
        self.add_media_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # 不拦截快捷键
        self.add_media_btn.clicked.connect(lambda: signal_bus.media_add_dialog_requested.emit())
        layout.addWidget(self.add_media_btn)

        layout.addSpacing(4)

        # 新窗口按钮
        self.new_window_btn = PushButton("＋ 新窗口", self)
        self.new_window_btn.setFixedHeight(32)
        self.new_window_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # 不拦截快捷键
        self.new_window_btn.clicked.connect(lambda: signal_bus.new_window_requested.emit())
        layout.addWidget(self.new_window_btn)

        layout.addSpacing(4)

        # 打开按钮
        self.open_btn = create_tool_button(FluentIcon.FOLDER, self, 32, "打开项目")
        self.open_btn.clicked.connect(self._on_open_project)
        layout.addWidget(self.open_btn)
        #layout.addSpacing(4)
        self.open_btn.hide()  # TODO: 待实现，0.1.0 临时隐藏

        # 保存按钮
        self.save_btn = create_tool_button(FluentIcon.SAVE, self, 32, "保存项目")
        self.save_btn.clicked.connect(self._on_save_project)
        layout.addWidget(self.save_btn)
        self.save_btn.hide()  # TODO: 待实现，0.1.0 临时隐藏

        layout.addSpacing(4)

        # 设置按钮
        self.settings_btn = create_tool_button(FluentIcon.SETTING, self, 32, "设置")
        self.settings_btn.clicked.connect(lambda: signal_bus.settings_requested.emit())
        layout.addWidget(self.settings_btn)

        layout.addSpacing(4)

        # 性能监控按钮
        self.debug_btn = create_tool_button(FluentIcon.STOP_WATCH, self, 32, "性能监控")
        self.debug_btn.clicked.connect(lambda: signal_bus.debug_monitor_requested.emit())
        layout.addWidget(self.debug_btn)

    def _on_view_mode_changed(self, mode: ViewMode):
        """视图模式切换"""
        self.view_mode_changed.emit(mode)
        signal_bus.view_mode_changed.emit(mode)

    def _on_open_project(self):
        """打开项目"""
        # TODO: 实现打开项目逻辑
        pass

    def _on_save_project(self):
        """保存项目"""
        # TODO: 实现保存项目逻辑
        pass

    def set_view_mode(self, mode: ViewMode):
        """设置当前视图模式"""
        if mode == ViewMode.SIDE_BY_SIDE:
            self.view_mode_segment.setCurrentItem("side_by_side")
        else:
            self.view_mode_segment.setCurrentItem("split_screen")

    def set_view_mode_enabled(self, enabled: bool):
        """设置视图模式切换是否可用"""
        self.view_mode_segment.setEnabled(enabled)
        # 禁用时设置透明度模拟置灰效果
        if enabled:
            self.view_mode_segment.setGraphicsEffect(None)
        else:
            effect = QGraphicsOpacityEffect(self.view_mode_segment)
            effect.setOpacity(0.5)
            self.view_mode_segment.setGraphicsEffect(effect)
