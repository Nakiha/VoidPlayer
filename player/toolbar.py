"""
ToolBar - 顶部工具栏
"""
from PySide6.QtWidgets import QWidget, QHBoxLayout, QSpacerItem, QSizePolicy
from PySide6.QtCore import Signal
from qfluentwidgets import (
    PushButton,
    PrimaryPushButton,
    TransparentToolButton,
    SegmentedWidget,
    FluentIcon,
)

from .viewport import ViewMode
from .widgets import create_tool_button


class ToolBar(QWidget):
    """顶部工具栏 - 视图模式切换和项目操作"""

    # 信号
    view_mode_changed = Signal(ViewMode)
    add_media_clicked = Signal()
    new_project_clicked = Signal()
    open_project_clicked = Signal()
    save_project_clicked = Signal()
    export_clicked = Signal()
    settings_clicked = Signal()
    help_clicked = Signal()
    debug_monitor_clicked = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedHeight(40)  # 32px 内容 + 4px * 2 边距
        self._setup_ui()

    def _setup_ui(self):
        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 4, 8, 4)
        layout.setSpacing(0)

        # 左侧: 视图模式切换
        self.view_mode_segment = SegmentedWidget(self)
        self.view_mode_segment.addItem("side_by_side", "并排", lambda: self._on_view_mode_changed(ViewMode.SIDE_BY_SIDE))
        self.view_mode_segment.addItem("split_screen", "分屏", lambda: self._on_view_mode_changed(ViewMode.SPLIT_SCREEN))
        self.view_mode_segment.setCurrentItem("side_by_side")
        self.view_mode_segment.setFixedHeight(32)  # 与添加媒体按钮同高

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
        self.add_media_btn.clicked.connect(self.add_media_clicked)
        layout.addWidget(self.add_media_btn)

        layout.addSpacing(4)

        # 新项目按钮
        self.new_project_btn = PushButton("＋ 新项目", self)
        self.new_project_btn.setFixedHeight(32)
        self.new_project_btn.clicked.connect(self.new_project_clicked)
        layout.addWidget(self.new_project_btn)

        layout.addSpacing(4)

        # 打开按钮
        self.open_btn = create_tool_button(FluentIcon.FOLDER, self, 32)
        self.open_btn.setToolTip("打开项目")
        self.open_btn.clicked.connect(self.open_project_clicked)
        layout.addWidget(self.open_btn)

        layout.addSpacing(4)

        # 保存按钮
        self.save_btn = create_tool_button(FluentIcon.SAVE, self, 32)
        self.save_btn.setToolTip("保存项目")
        self.save_btn.clicked.connect(self.save_project_clicked)
        layout.addWidget(self.save_btn)

        layout.addSpacing(4)

        # 导出按钮
        self.export_btn = create_tool_button(FluentIcon.SHARE, self, 32)
        self.export_btn.setToolTip("导出")
        self.export_btn.clicked.connect(self.export_clicked)
        layout.addWidget(self.export_btn)

        layout.addSpacing(4)

        # 设置按钮
        self.settings_btn = create_tool_button(FluentIcon.SETTING, self, 32)
        self.settings_btn.setToolTip("设置")
        self.settings_btn.clicked.connect(self.settings_clicked)
        layout.addWidget(self.settings_btn)

        layout.addSpacing(4)

        # 性能监控按钮
        self.debug_btn = create_tool_button(FluentIcon.STOP_WATCH, self, 32)
        self.debug_btn.setToolTip("性能监控")
        self.debug_btn.clicked.connect(self.debug_monitor_clicked)
        layout.addWidget(self.debug_btn)

        layout.addSpacing(4)

        # 帮助按钮
        self.help_btn = create_tool_button(FluentIcon.HELP, self, 32)
        self.help_btn.setToolTip("帮助")
        self.help_btn.clicked.connect(self.help_clicked)
        layout.addWidget(self.help_btn)

    def _on_view_mode_changed(self, mode: ViewMode):
        """视图模式切换"""
        self.view_mode_changed.emit(mode)

    def set_view_mode(self, mode: ViewMode):
        """设置当前视图模式"""
        if mode == ViewMode.SIDE_BY_SIDE:
            self.view_mode_segment.setCurrentItem("side_by_side")
        else:
            self.view_mode_segment.setCurrentItem("split_screen")
