"""
MainWindow - 主窗口
"""
from typing import Optional
from PySide6.QtWidgets import QWidget, QVBoxLayout
from PySide6.QtCore import Qt
from qfluentwidgets import isDarkTheme, FluentIcon

from .view_mode import ViewMode
from .toolbar import ToolBar
from .viewport_panel import ViewportPanel
from .media_info_bar import MediaInfoBar
from .controls_bar import ControlsBar
from .timeline_area import TimelineArea
from .theme_utils import get_color_hex, ColorKey
from .debug_monitor import DebugMonitorWindow
from .config import config, Profile


class MainWindow(QWidget):
    """主窗口 - 整体布局协调"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._sources: list[str] = []
        self._view_mode = ViewMode.SIDE_BY_SIDE
        self._is_playing = False
        self._debug_monitor: DebugMonitorWindow | None = None
        self._setup_ui()
        self._connect_signals()
        self._load_demo_data()

    def _setup_ui(self):
        self.setWindowTitle("VoidPlayer - 视频对比播放器")
        self.setMinimumSize(1200, 800)
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

        # 2. 视频预览区域 (flex: 1)
        self.viewport_panel = ViewportPanel(self)
        self.main_layout.addWidget(self.viewport_panel, 1)

        # 3. 媒体信息条 (32px)
        self.media_info_bar = MediaInfoBar(self)
        self.main_layout.addWidget(self.media_info_bar)

        # 媒体信息条和播放控制条之间的间距
        self.main_layout.addSpacing(4)

        # 4. 播放控制条 (42px)
        self.controls_bar = ControlsBar(self)
        self.main_layout.addWidget(self.controls_bar)

        # 5. 时间轴轨道区域 (动态高度)
        self.timeline_area = TimelineArea(self)
        self.main_layout.addWidget(self.timeline_area)

    def _connect_signals(self):
        """连接信号"""
        # 工具栏信号
        self.toolbar.view_mode_changed.connect(self.set_view_mode)
        self.toolbar.add_media_clicked.connect(self._on_add_media)

        # 调试监控 (仅在非性能模式下启用)
        if config.profile != Profile.PERF:
            self.toolbar.debug_monitor_clicked.connect(self._show_debug_monitor)
        else:
            self.toolbar.debug_btn.hide()

        # 媒体信息条信号
        self.media_info_bar.media_remove_clicked.connect(self.remove_media)
        self.media_info_bar.media_changed.connect(self._on_media_changed)

        # 控制条信号
        self.controls_bar.play_clicked.connect(self.play)
        self.controls_bar.pause_clicked.connect(self.pause)
        self.controls_bar.seek_requested.connect(self.seek_to)

        # 时间轴信号
        self.timeline_area.track_remove_clicked.connect(self.remove_media)
        self.timeline_area.track_offset_changed.connect(self.set_sync_offset)

    def _load_demo_data(self):
        """加载演示数据"""
        # 添加演示媒体
        self.add_media("CityHall_1920x1080")
        self.add_media("UshaikaRiverEmb_1920x1080")

    def _on_add_media(self):
        """添加媒体按钮点击"""
        # 演示: 添加一个新媒体
        new_media = f"Video_{len(self._sources) + 1}"
        self.add_media(new_media)

    def _show_debug_monitor(self):
        """显示性能监控窗口"""
        if self._debug_monitor is None:
            self._debug_monitor = DebugMonitorWindow(
                None, auto_tracemalloc=(config.profile == Profile.DEBUG)
            )
        self._debug_monitor.show()
        self._debug_monitor.raise_()
        self._debug_monitor.activateWindow()

    def _on_media_changed(self, item_index: int, media_index: int):
        """媒体选择改变时的回调"""
        # item_index: 哪个媒体项 (0=左, 1=右)
        # media_index: 选择了哪个源
        # TODO: 切换 VideoPlaceholder 中的画面
        pass

    # ========== 公共 API ==========

    def load_sources(self, sources: list[str]):
        """加载媒体源列表"""
        self._sources.clear()
        self.timeline_area.clear_tracks()

        # 设置媒体信息栏的源列表
        self.media_info_bar.set_sources(sources)
        self.media_info_bar.set_media_count(len(sources))

        for i, source in enumerate(sources):
            self._sources.append(source)
            self.timeline_area.add_track(i, source)
            self.media_info_bar.set_media_name(i, source)

    def add_media(self, path: str):
        """添加单个媒体"""
        index = len(self._sources)
        self._sources.append(path)

        # 添加源到媒体信息栏的源列表
        self.media_info_bar.add_source(path)
        # 添加媒体项并设置当前源
        self.media_info_bar.add_media_item(path)

        # 添加轨道
        self.timeline_area.add_track(index, path)

    def remove_media(self, index: int):
        """移除媒体"""
        if 0 <= index < len(self._sources):
            source = self._sources.pop(index)
            self.timeline_area.remove_track(index)
            # 移除媒体项和源
            self.media_info_bar.remove_media_item(index)
            self.media_info_bar.remove_source(source)

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
        # 清空媒体信息栏
        self.media_info_bar.set_sources([])
        self.media_info_bar.set_media_count(0)

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
