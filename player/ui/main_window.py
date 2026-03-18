"""
MainWindow - 主窗口
"""
import sys
import subprocess
from typing import Optional
from PySide6.QtWidgets import QWidget, QVBoxLayout, QFileDialog
from PySide6.QtCore import Qt, QMimeData, QTimer
from PySide6.QtGui import QDragEnterEvent, QDropEvent
from qfluentwidgets import isDarkTheme, FluentIcon

from .viewport import ViewMode, ViewportPanel
from .toolbar import ToolBar
from .controls_bar import ControlsBar
from .timeline_area import TimelineArea
from ..theme_utils import get_color_hex, ColorKey
from ..core.debug_monitor import DebugMonitorWindow
from ..core.config import config, Profile
from .widgets import HighlightSplitter
from ..core.track_manager import TrackManager
from ..core.decoder_pool import DecoderPool
from ..core.shortcuts import ShortcutManager, ShortcutAction


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
        self._view_mode = ViewMode.SIDE_BY_SIDE
        self._is_playing = False
        self._debug_monitor: DebugMonitorWindow | None = None
        self._launch_args = launch_args or []

        # 唯一数据源
        self._track_manager = TrackManager(self)

        # 解码器池
        self._decoder_pool = DecoderPool(self)

        # 快捷键管理器
        self._shortcut_manager = ShortcutManager(self)

        self._setup_ui()
        self._connect_signals()
        self._setup_shortcuts()
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
        QTimer.singleShot(0, self._init_splitter_sizes)

    def _connect_signals(self):
        """连接信号"""
        # TrackManager 信号 -> 更新各组件
        self._track_manager.source_added.connect(self._on_source_added)
        self._track_manager.source_removed.connect(self._on_source_removed)
        self._track_manager.sources_swapped.connect(self._on_sources_swapped)
        self._track_manager.sources_reordered.connect(self._on_sources_reordered)
        self._track_manager.sources_reset.connect(self._on_sources_reset)

        # 工具栏信号
        self.toolbar.view_mode_changed.connect(self.set_view_mode)
        self.toolbar.add_media_clicked.connect(self._on_add_media)
        self.toolbar.new_window_clicked.connect(self._on_new_window)

        # 调试监控 (仅在非性能模式下启用)
        if config.profile != Profile.PERF:
            self.toolbar.debug_monitor_clicked.connect(self._show_debug_monitor)
        else:
            self.toolbar.debug_btn.hide()

        # 视口面板信号
        self.viewport_panel.media_remove_clicked.connect(self.remove_media)
        self.viewport_panel.media_swap_requested.connect(self._track_manager.swap_sources)

        # 控制条信号
        self.controls_bar.play_clicked.connect(self.play)
        self.controls_bar.pause_clicked.connect(self.pause)
        self.controls_bar.seek_requested.connect(self.seek_to)

        # 时间轴信号
        self.timeline_area.track_remove_clicked.connect(self.remove_media)
        self.timeline_area.track_offset_changed.connect(self.set_sync_offset)
        self.timeline_area.track_move_requested.connect(self._track_manager.move_source)

        # 分割器信号
        self.v_splitter.splitterMoved.connect(self._on_splitter_moved)

        # 时间轴扩展请求
        self.timeline_area.expand_requested.connect(self._on_timeline_expand_requested)

        # 解码器池信号
        self._decoder_pool.duration_changed.connect(self._on_duration_changed)
        self._decoder_pool.position_changed.connect(self._on_position_changed)
        self._decoder_pool.frame_ready.connect(self._on_frame_ready)
        self._decoder_pool.eof_reached.connect(self._on_eof_reached)
        self._decoder_pool.error_occurred.connect(self._on_decoder_error)

        # GL 控件初始化完成信号
        self.viewport_panel.gl_initialized.connect(self._on_gl_initialized)

    def _setup_shortcuts(self):
        """设置快捷键"""
        self._shortcut_manager.setup(self)

        # 播放控制
        self._shortcut_manager.bind(ShortcutAction.PLAY_PAUSE, self._toggle_play_pause)
        self._shortcut_manager.bind(ShortcutAction.PREV_FRAME, self._prev_frame)
        self._shortcut_manager.bind(ShortcutAction.NEXT_FRAME, self._next_frame)
        self._shortcut_manager.bind(ShortcutAction.SEEK_FORWARD, self._seek_forward)
        self._shortcut_manager.bind(ShortcutAction.SEEK_BACKWARD, self._seek_backward)
        self._shortcut_manager.bind(ShortcutAction.TOGGLE_LOOP, self._toggle_loop)
        self._shortcut_manager.bind(ShortcutAction.TOGGLE_FULLSCREEN, self._toggle_fullscreen)

        # 速度控制
        self._shortcut_manager.bind(ShortcutAction.SPEED_UP, self._speed_up)
        self._shortcut_manager.bind(ShortcutAction.SPEED_DOWN, self._speed_down)
        self._shortcut_manager.bind(ShortcutAction.SPEED_RESET, self._speed_reset)

        # 缩放控制
        self._shortcut_manager.bind(ShortcutAction.ZOOM_IN, self._zoom_in)
        self._shortcut_manager.bind(ShortcutAction.ZOOM_OUT, self._zoom_out)
        self._shortcut_manager.bind(ShortcutAction.ZOOM_RESET, self._zoom_reset)

        # 项目操作
        self._shortcut_manager.bind(ShortcutAction.ADD_MEDIA, self._on_add_media)
        self._shortcut_manager.bind(ShortcutAction.NEW_WINDOW, self._on_new_window)
        self._shortcut_manager.bind(ShortcutAction.TOGGLE_DEBUG_MONITOR, self._show_debug_monitor)

    def _seek_forward(self):
        """前进 5 秒"""
        current = self._decoder_pool.position_ms
        self.seek_to(current + 5000)

    def _seek_backward(self):
        """后退 5 秒"""
        current = self._decoder_pool.position_ms
        self.seek_to(max(0, current - 5000))

    def _toggle_play_pause(self):
        """切换播放/暂停"""
        if self._is_playing:
            self.pause()
        else:
            self.play()

    def _prev_frame(self):
        """上一帧"""
        self._decoder_pool.prev_frame()

    def _next_frame(self):
        """下一帧"""
        self._decoder_pool.next_frame()

    def _toggle_loop(self):
        """切换循环"""
        self.controls_bar.loop_btn.toggle()

    def _toggle_fullscreen(self):
        """切换全屏"""
        if self.isFullScreen():
            self.showNormal()
        else:
            self.showFullScreen()

    def _speed_up(self):
        """加速"""
        idx = self.controls_bar.speed_combo.currentIndex()
        if idx < self.controls_bar.speed_combo.count() - 1:
            self.controls_bar.speed_combo.setCurrentIndex(idx + 1)

    def _speed_down(self):
        """减速"""
        idx = self.controls_bar.speed_combo.currentIndex()
        if idx > 0:
            self.controls_bar.speed_combo.setCurrentIndex(idx - 1)

    def _speed_reset(self):
        """重置速度"""
        self.controls_bar.speed_combo.setCurrentIndex(2)  # 1x

    def _zoom_in(self):
        """放大"""
        idx = self.controls_bar.zoom_combo.currentIndex()
        if idx < self.controls_bar.zoom_combo.count() - 1:
            self.controls_bar.zoom_combo.setCurrentIndex(idx + 1)

    def _zoom_out(self):
        """缩小"""
        idx = self.controls_bar.zoom_combo.currentIndex()
        if idx > 0:
            self.controls_bar.zoom_combo.setCurrentIndex(idx - 1)

    def _zoom_reset(self):
        """重置缩放"""
        self.controls_bar.zoom_combo.setCurrentIndex(2)  # 100%

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
        """时间轴扩展请求"""
        sizes = self.v_splitter.sizes()
        if len(sizes) >= 2:
            total = sum(sizes)
            if total > 0:
                max_timeline_height = int(total * 0.4)
                target_height = min(required_height, max_timeline_height)
                current_height = sizes[1]
                if current_height < target_height:
                    new_top_height = total - target_height
                    self.v_splitter.setSizes([new_top_height, target_height])

    # ========== TrackManager 信号回调 ==========

    def _on_source_added(self, index: int, source: str):
        """源添加回调"""
        # 添加到解码器池并探测媒体信息
        if self._decoder_pool.add_track(index, source):
            track_state = self._decoder_pool.get_track_state(index)
            if track_state and track_state.media_info:
                # 传递媒体信息给 timeline
                self.timeline_area.add_track(index, source, track_state.media_info)
            else:
                self.timeline_area.add_track(index, source)

            # 如果 GL 已初始化，立即初始化解码器
            if self.viewport_panel.gl_widget.is_gl_initialized:
                self._decoder_pool.initialize_decoder(index)
                self.viewport_panel.gl_widget.set_decoders(self._decoder_pool.get_decoders())
        else:
            # 探测失败也添加 track (显示错误)
            self.timeline_area.add_track(index, source)

        self.viewport_panel.add_slot(source)
        self._update_view_mode_enabled()

    def _on_source_removed(self, index: int):
        """源移除回调"""
        self._decoder_pool.remove_track(index)
        self.timeline_area.remove_track(index)
        self.viewport_panel.remove_slot(index)
        self._update_view_mode_enabled()

    def _on_sources_swapped(self, index1: int, index2: int):
        """源交换回调"""
        # 交换后需要同步更新 UI 显示
        self.viewport_panel.on_sources_swapped(index1, index2)
        self.timeline_area.reorder_track(index1, index2)

    def _on_sources_reordered(self, old_index: int, new_index: int):
        """源重排序回调"""
        self.viewport_panel.on_source_moved(old_index, new_index)
        self.timeline_area.reorder_track(old_index, new_index)

    def _on_sources_reset(self):
        """源重置回调"""
        self._decoder_pool.clear()
        sources = self._track_manager.sources()
        self.viewport_panel.set_sources(sources)
        self.timeline_area.clear_tracks()
        for i, source in enumerate(sources):
            if self._decoder_pool.add_track(i, source):
                track_state = self._decoder_pool.get_track_state(i)
                if track_state and track_state.media_info:
                    self.timeline_area.add_track(i, source, track_state.media_info)
                else:
                    self.timeline_area.add_track(i, source)

                # 如果 GL 已初始化，初始化解码器
                if self.viewport_panel.gl_widget.is_gl_initialized:
                    self._decoder_pool.initialize_decoder(i)
            else:
                self.timeline_area.add_track(i, source)

        # 更新 GL 控件的解码器列表
        if self.viewport_panel.gl_widget.is_gl_initialized:
            self.viewport_panel.gl_widget.set_decoders(self._decoder_pool.get_decoders())

        self._update_view_mode_enabled()

    # ========== 用户操作 ==========

    def _load_initial_files(self, files: list[str]):
        """加载初始文件"""
        for file_path in files:
            self.add_media(file_path)

    def _on_add_media(self):
        """添加媒体按钮点击"""
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

        exe = sys.executable
        script = sys.argv[0]
        cmd = [exe, script] + filtered_args
        subprocess.Popen(cmd)

    def dragEnterEvent(self, event: QDragEnterEvent):
        """拖入事件"""
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent):
        """放下事件"""
        for url in event.mimeData().urls():
            file_path = url.toLocalFile()
            if file_path:
                self.add_media(file_path)
        event.acceptProposedAction()

    def _update_view_mode_enabled(self):
        """更新视图模式切换的可用状态"""
        self.toolbar.set_view_mode_enabled(self._track_manager.count() > 0)

    def _show_debug_monitor(self):
        """显示性能监控窗口"""
        if self._debug_monitor is None:
            self._debug_monitor = DebugMonitorWindow(
                None, auto_tracemalloc=(config.profile == Profile.DEBUG)
            )

        if self._debug_monitor.isMinimized():
            self._debug_monitor.showNormal()
        else:
            self._debug_monitor.show()

        self._debug_monitor.raise_()
        self._debug_monitor.activateWindow()

    # ========== 解码器池回调 ==========

    def _on_duration_changed(self, duration_ms: int):
        """时长变化"""
        self.controls_bar.set_duration(duration_ms)

    def _on_position_changed(self, position_ms: int):
        """播放位置变化"""
        if self._decoder_pool.duration_ms > 0:
            position = position_ms / self._decoder_pool.duration_ms
            self.timeline_area.update_playhead(position)
        self.controls_bar.set_position(position_ms)

    def _on_frame_ready(self):
        """帧就绪 - 更新 GL 控件"""
        self.viewport_panel.gl_widget.update()

    def _on_eof_reached(self):
        """到达末尾"""
        self._is_playing = False
        self.controls_bar.set_playing(False)

    def _on_decoder_error(self, track_index: int, message: str):
        """解码器错误"""
        print(f"Decoder error (track {track_index}): {message}")

    def _on_gl_initialized(self):
        """OpenGL 上下文初始化完成 - 可以初始化解码器了"""
        # 初始化所有已有轨道的解码器
        for i in range(self._track_manager.count()):
            source = self._track_manager.get(i)
            if source:
                self._init_decoder(i, source)

    def _init_decoder(self, index: int, source: str):
        """初始化单个解码器"""
        # 添加到解码器池
        if not self._decoder_pool.add_track(index, source):
            return

        # 初始化解码器 (需要 GL 上下文)
        if self.viewport_panel.gl_widget.is_gl_initialized:
            self._decoder_pool.initialize_decoder(index)
            # 将解码器列表传递给 GL 控件
            self.viewport_panel.gl_widget.set_decoders(self._decoder_pool.get_decoders())

    # ========== 公共 API ==========

    def add_media(self, path: str):
        """添加媒体"""
        self._track_manager.add_source(path)

    def remove_media(self, index: int):
        """移除媒体"""
        self._track_manager.remove_source(index)

    def set_sync_offset(self, index: int, offset_ms: int):
        """设置时间偏移"""
        self._decoder_pool.set_offset(index, offset_ms)

    def set_view_mode(self, mode: ViewMode):
        """切换视图模式"""
        self._view_mode = mode
        self.viewport_panel.set_view_mode(mode)

    def play(self):
        """开始播放"""
        self._is_playing = True
        self.controls_bar.set_playing(True)
        self._decoder_pool.play()

    def pause(self):
        """暂停播放"""
        self._is_playing = False
        self.controls_bar.set_playing(False)
        self._decoder_pool.pause()

    def seek_to(self, timestamp_ms: int):
        """跳转到指定时间"""
        self._decoder_pool.seek_to(timestamp_ms)

    def new_project(self):
        """新建项目"""
        self._track_manager.clear()

    def load_sources(self, sources: list[str]):
        """加载媒体源列表"""
        self._track_manager.set_sources(sources)

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
