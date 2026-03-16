"""
MainWindow - 主窗口
"""
import sys
import subprocess
from typing import Optional
from PySide6.QtWidgets import QWidget, QVBoxLayout, QFileDialog
from PySide6.QtCore import Qt, QMimeData
from PySide6.QtGui import QDragEnterEvent, QDropEvent
from qfluentwidgets import isDarkTheme, FluentIcon

from PySide6.QtWidgets import QWidget, QVBoxLayout, QFileDialog
from PySide6.QtCore import Qt, QMimeData
from PySide6.QtGui import QDragEnterEvent, QDropEvent
from qfluentwidgets import isDarkTheme, FluentIcon

from .viewport import ViewMode, ViewportPanel
from .toolbar import ToolBar
from .controls_bar import ControlsBar
from .timeline_area import TimelineArea
from .theme_utils import get_color_hex, ColorKey
from .debug_monitor import DebugMonitorWindow
from .config import config, Profile
from .widgets import HighlightSplitter


class MainWindow(QWidget):
    """主窗口 - 整体布局协调"""

    # 启动新窗口时要排除的参数黑名单
    NEW_WINDOW_EXCLUDE_ARGS = {"-i", "--input"}

    def __init__(
        self,
        initial_files: Optional[list[str]] = None,
        launch_args: Optional[list[str]] = None,
        parent=None
    ):
        super().__init__(parent)
        self._sources: list[str] = []
        self._view_mode = ViewMode.SIDE_BY_SIDE
        self._is_playing = False
        self._debug_monitor: DebugMonitorWindow | None = None
        self._launch_args = launch_args or []
        self._setup_ui()
        self._connect_signals()
        self._load_initial_files(initial_files or [])

    def _setup_ui(self):
        self.setWindowTitle("VoidPlayer - 视频对比播放器")
        self.setMinimumSize(520, 360)
        self.resize(520, 360)
        self.setAcceptDrops(True)
        self._update_style()

    def _update_style(self):
        """更新样式 (主题变化时调用)"""
        self.setStyleSheet(f"""
            QWidget {{
                background-color: {get_color_hex(ColorKey.BG_BASE)};
                color: {get_color_hex(ColorKey.TEXT_PRIMARY)};
            }}
        """)

        # 主布局
        self.main_layout = QVBoxLayout(self)
        self.main_layout.setContentsMargins(0, 0, 0, 0)
        self.main_layout.setSpacing(0)

        # 1. 工具栏 (50px)
        self.toolbar = ToolBar(self)
        self.main_layout.addWidget(self.toolbar)

        # 2. 垂直分割器 - 分隔上部区域和 timeline_area
        self.v_splitter = HighlightSplitter(Qt.Orientation.Vertical, self)
        self.v_splitter.setHandleWidth(1)
        self.v_splitter.setChildrenCollapsible(False)

        # 2.1 上部容器 (viewport_panel + controls_bar)
        self.top_container = QWidget(self.v_splitter)
        self.top_layout = QVBoxLayout(self.top_container)
        self.top_layout.setContentsMargins(0, 0, 0, 0)
        self.top_layout.setSpacing(0)

        # 视频预览区域（包含媒体信息）
        self.viewport_panel = ViewportPanel(self.top_container)
        self.top_layout.addWidget(self.viewport_panel, 1)

        # 播放控制条 (42px)
        self.controls_bar = ControlsBar(self.top_container)
        self.top_layout.addWidget(self.controls_bar)

        self.v_splitter.addWidget(self.top_container)

        # 2.2 时间轴轨道区域 (可压缩，最小 0px)
        self.timeline_area = TimelineArea(self.v_splitter)
        self.timeline_area.setMinimumHeight(0)
        self.v_splitter.addWidget(self.timeline_area)

        # 设置分割器初始比例：上部占主要空间
        self.main_layout.addWidget(self.v_splitter, 1)

        # 延迟设置初始大小比例
        from PySide6.QtCore import QTimer
        QTimer.singleShot(0, self._init_splitter_sizes)

    def _connect_signals(self):
        """连接信号"""
        # 工具栏信号
        self.toolbar.view_mode_changed.connect(self.set_view_mode)
        self.toolbar.add_media_clicked.connect(self._on_add_media)
        self.toolbar.new_window_clicked.connect(self._on_new_window)

        # 调试监控 (仅在非性能模式下启用)
        if config.profile != Profile.PERF:
            self.toolbar.debug_monitor_clicked.connect(self._show_debug_monitor)
        else:
            self.toolbar.debug_btn.hide()

        # 视口面板信号（包含媒体选择和移除）
        self.viewport_panel.media_remove_clicked.connect(self.remove_media)
        self.viewport_panel.media_changed.connect(self._on_media_changed)

        # 控制条信号
        self.controls_bar.play_clicked.connect(self.play)
        self.controls_bar.pause_clicked.connect(self.pause)
        self.controls_bar.seek_requested.connect(self.seek_to)

        # 时间轴信号
        self.timeline_area.track_remove_clicked.connect(self.remove_media)
        self.timeline_area.track_offset_changed.connect(self.set_sync_offset)

        # 分割器信号 - 用于限制 timeline_area 最大高度
        self.v_splitter.splitterMoved.connect(self._on_splitter_moved)

        # 时间轴扩展请求
        self.timeline_area.expand_requested.connect(self._on_timeline_expand_requested)

    def _init_splitter_sizes(self):
        """初始化分割器大小比例"""
        total_height = self.v_splitter.height()
        if total_height > 0:
            # 上部区域占大部分，timeline_area 使用其最大高度
            timeline_max = self.timeline_area.maximumHeight()
            top_height = max(total_height - timeline_max, total_height // 2)
            self.v_splitter.setSizes([top_height, total_height - top_height])

    def _on_splitter_moved(self, pos: int, index: int):
        """分割器移动 - 限制 timeline_area 最大高度不超过窗体 40%"""
        sizes = self.v_splitter.sizes()
        if len(sizes) >= 2:
            total = sum(sizes)
            if total > 0:
                max_timeline_height = int(total * 0.4)
                if sizes[1] > max_timeline_height:
                    excess = sizes[1] - max_timeline_height
                    self.v_splitter.setSizes([sizes[0] + excess, max_timeline_height])

    def _on_timeline_expand_requested(self, required_height: int):
        """时间轴扩展请求 - 自动调整 splitter 使 timeline_area 获得足够高度"""
        sizes = self.v_splitter.sizes()
        if len(sizes) >= 2:
            total = sum(sizes)
            if total > 0:
                # 最大高度为窗体 40%
                max_timeline_height = int(total * 0.4)
                # 目标高度：不超过最大限制
                target_height = min(required_height, max_timeline_height)

                current_height = sizes[1]
                # 只有当前高度小于目标高度时才扩展
                if current_height < target_height:
                    new_top_height = total - target_height
                    self.v_splitter.setSizes([new_top_height, target_height])

    def _load_initial_files(self, files: list[str]):
        """加载初始文件"""
        for file_path in files:
            self.add_media(file_path)

    def _on_add_media(self):
        """添加媒体按钮点击 - 打开文件选择器"""
        files, _ = QFileDialog.getOpenFileNames(
            self,
            "选择媒体文件",
            "",
            "所有文件 (*.*)"
        )
        for file_path in files:
            self.add_media(file_path)

    def _on_new_window(self):
        """启动新的 VoidPlayer 窗口"""
        # 过滤掉黑名单中的参数
        filtered_args = []
        skip_next = False
        for arg in self._launch_args:
            if skip_next:
                skip_next = False
                continue
            if arg in self.NEW_WINDOW_EXCLUDE_ARGS:
                skip_next = True
                continue
            filtered_args.append(arg)

        # 启动新进程
        exe = sys.executable
        script = sys.argv[0]
        cmd = [exe, script] + filtered_args
        subprocess.Popen(cmd)

    def dragEnterEvent(self, event: QDragEnterEvent):
        """拖入事件 - 接受文件拖入"""
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent):
        """放下事件 - 处理拖入的文件"""
        for url in event.mimeData().urls():
            file_path = url.toLocalFile()
            if file_path:
                self.add_media(file_path)
        event.acceptProposedAction()

    def _update_view_mode_enabled(self):
        """根据文件数量更新视图模式切换的可用状态"""
        self.toolbar.set_view_mode_enabled(len(self._sources) > 0)

    def _show_debug_monitor(self):
        """显示性能监控窗口"""
        if self._debug_monitor is None:
            self._debug_monitor = DebugMonitorWindow(
                None, auto_tracemalloc=(config.profile == Profile.DEBUG)
            )

        # 如果窗口最小化，恢复窗口状态
        if self._debug_monitor.isMinimized():
            self._debug_monitor.showNormal()
        else:
            self._debug_monitor.show()

        self._debug_monitor.raise_()
        self._debug_monitor.activateWindow()

    def _on_media_changed(self, slot_index: int, media_index: int):
        """媒体选择改变时的回调"""
        # slot_index: 哪个槽位 (0=左, 1=右, ...)
        # media_index: 选择了哪个源
        # TODO: 切换 VideoPlaceholder 中的画面
        pass

    # ========== 公共 API ==========

    def load_sources(self, sources: list[str]):
        """加载媒体源列表"""
        self._sources.clear()
        self.timeline_area.clear_tracks()

        # 设置视口面板的源列表
        self.viewport_panel.set_sources(sources)

        for i, source in enumerate(sources):
            self._sources.append(source)
            self.timeline_area.add_track(i, source)

    def add_media(self, path: str):
        """添加单个媒体"""
        index = len(self._sources)
        self._sources.append(path)

        # 添加源到视口面板
        self.viewport_panel.add_source(path)

        # 添加轨道
        self.timeline_area.add_track(index, path)

        # 更新视图模式切换状态
        self._update_view_mode_enabled()

    def remove_media(self, index: int):
        """移除媒体"""
        if 0 <= index < len(self._sources):
            self._sources.pop(index)
            self.timeline_area.remove_track(index)
            # 移除视口面板中的源
            self.viewport_panel.remove_source(index)

            # 更新视图模式切换状态
            self._update_view_mode_enabled()

    def set_sync_offset(self, index: int, offset_ms: int):
        """设置时间偏移"""
        # TODO: 实现偏移逻辑
        pass

    def set_view_mode(self, mode: ViewMode):
        """切换视图模式"""
        self._view_mode = mode
        self.viewport_panel.set_view_mode(mode)

    def play(self):
        """开始播放"""
        self._is_playing = True
        self.controls_bar.set_playing(True)

    def pause(self):
        """暂停播放"""
        self._is_playing = False
        self.controls_bar.set_playing(False)

    def seek_to(self, timestamp_ms: int):
        """跳转到指定时间"""
        # 更新播放头位置
        duration = 9600  # 演示时长
        if duration > 0:
            position = timestamp_ms / duration
            self.timeline_area.update_playhead(position)

    def new_project(self):
        """新建项目"""
        self._sources.clear()
        self.timeline_area.clear_tracks()
        # 清空视口面板
        self.viewport_panel.set_sources([])

    def open_project(self, path: str):
        """打开项目"""
        # TODO: 实现项目加载
        pass

    def save_project(self, path: str):
        """保存项目"""
        # TODO: 实现项目保存
        pass

    def export_report(self, path: str):
        """导出评测报告"""
        # TODO: 实现报告导出
        pass
