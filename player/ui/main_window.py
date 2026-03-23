"""
MainWindow - 主窗口
"""
import sys
import subprocess
import time
from typing import Optional
from PySide6.QtWidgets import QWidget, QVBoxLayout, QFileDialog
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QDragEnterEvent, QDropEvent
from .viewport import ViewMode, ViewportPanel
from .toolbar import ToolBar
from .controls_bar import ControlsBar
from .timeline_area import TimelineArea
from .theme_utils import get_color_hex, ColorKey
from .windows import MemoryWindow, StatsWindow, SettingsWindow
from ..core.config import config, Profile
from .widgets import HighlightSplitter
from ..core.track_manager import TrackManager
from ..core.decoder_pool import DecoderPool
from ..core.playback_controller import PlaybackController
from ..core.shortcuts import ShortcutManager
from ..core.actions import ActionDispatcher, create_action_registry
from ..core.actions.viewport_zoom import ViewportZoomAction
from ..core.actions.viewport_pan import ViewportPanAction
from ..core.viewport import ViewportManager
from ..core.diagnostics.automation import AutomationController
from ..core.signal_bus import signal_bus
from ..core.logging_config import get_logger
from ..core.diagnostics import DiagnosticsManager


class MainWindow(QWidget):
    """主窗口 - 整体布局协调"""

    # 启动新窗口时要排除的参数黑名单
    NEW_WINDOW_EXCLUDE_ARGS = {"-i", "--input"}

    def __init__(
        self,
        initial_files: Optional[list[str]] = None,
        auto_play: bool = False,
        launch_args: Optional[list[str]] = None,
        mock_script: Optional[str] = None,
        parent=None
    ):
        super().__init__(parent)
        self._view_mode = ViewMode.SIDE_BY_SIDE
        self._is_playing = False
        self._memory_window: MemoryWindow | None = None
        self._settings_window: SettingsWindow | None = None
        self._launch_args = launch_args or []
        self._auto_play = auto_play
        self._mock_script = mock_script

        # 核心组件
        self._track_manager = TrackManager(self)
        self._decoder_pool = DecoderPool(self)
        self._playback_controller = PlaybackController(self._decoder_pool, self)
        self._shortcut_manager = ShortcutManager(self)

        # Viewport 管理
        self._viewport_manager = ViewportManager(self)
        self._viewport_zoom_action = ViewportZoomAction(self._viewport_manager, self)
        self._viewport_pan_action = ViewportPanAction(self._viewport_manager, self)

        # 动作系统
        self._action_dispatcher = ActionDispatcher(self)
        self._automation_controller: AutomationController | None = None

        # 诊断模块
        self._diagnostics_manager = DiagnosticsManager(self._decoder_pool, self)

        self._setup_ui()
        self._connect_internal_signals()
        self._setup_diagnostics()
        self._setup_action_system()
        self._setup_shortcuts()
        self._connect_signal_bus()
        self._load_initial_files(initial_files or [])
        self._setup_automation()

    # ========== UI 布局 ==========

    def _setup_ui(self):
        """设置 UI 布局"""
        self.setWindowTitle("VoidPlayer - 视频对比播放器")
        self.setMinimumSize(520, 360)
        self.resize(520, 360)
        self.setAcceptDrops(True)

        # 样式
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

        # 2. 垂直分割器
        self.v_splitter = HighlightSplitter(Qt.Orientation.Vertical, self)
        self.v_splitter.setHandleWidth(1)
        self.v_splitter.setChildrenCollapsible(False)

        # 2.1 上部容器
        self.top_container = QWidget(self.v_splitter)
        self.top_layout = QVBoxLayout(self.top_container)
        self.top_layout.setContentsMargins(0, 0, 0, 0)
        self.top_layout.setSpacing(0)

        self.viewport_panel = ViewportPanel(self.top_container)
        self.top_layout.addWidget(self.viewport_panel, 1)

        self.controls_bar = ControlsBar(self.top_container)
        self.top_layout.addWidget(self.controls_bar)

        self.v_splitter.addWidget(self.top_container)

        # 2.2 时间轴区域
        self.timeline_area = TimelineArea(self.v_splitter)
        self.timeline_area.setMinimumHeight(0)
        self.v_splitter.addWidget(self.timeline_area)

        self.main_layout.addWidget(self.v_splitter, 1)

        # 延迟设置分割器比例
        QTimer.singleShot(0, self._init_splitter_sizes)

        # 调试按钮 (仅非性能模式)
        if config.profile == Profile.PERF:
            self.toolbar.debug_btn.hide()

    def _init_splitter_sizes(self):
        """初始化分割器大小比例"""
        total_height = self.v_splitter.height()
        if total_height > 0:
            timeline_max = self.timeline_area.maximumHeight()
            top_height = max(total_height - timeline_max, total_height // 2)
            self.v_splitter.setSizes([top_height, total_height - top_height])

    # ========== 内部信号连接 ==========

    def _connect_internal_signals(self):
        """连接内部组件信号"""
        tm = self._track_manager
        dp = self._decoder_pool

        # TrackManager -> 更新 UI
        tm.source_added.connect(self._on_source_added)
        tm.source_removed.connect(self._on_source_removed)
        tm.sources_swapped.connect(self._on_sources_swapped)
        tm.sources_reordered.connect(self._on_sources_reordered)
        tm.sources_reset.connect(self._on_sources_reset)

        # 工具栏
        self.toolbar.view_mode_changed.connect(self.set_view_mode)
        self.toolbar.add_media_clicked.connect(self._on_add_media)
        self.toolbar.new_window_clicked.connect(self._on_new_window)
        if config.profile != Profile.PERF:
            self.toolbar.debug_monitor_clicked.connect(self._show_memory_window)

        # 设置
        self.toolbar.settings_clicked.connect(self._show_settings_window)

        # 视口面板
        self.viewport_panel.media_remove_clicked.connect(self.remove_media)
        self.viewport_panel.media_swap_requested.connect(tm.swap_sources)
        self.viewport_panel.gl_initialized.connect(self._on_gl_initialized)

        # 控制条
        self.controls_bar.play_clicked.connect(self.play)
        self.controls_bar.pause_clicked.connect(self.pause)
        self.controls_bar.seek_requested.connect(self.seek_to)
        self.controls_bar.precise_seek_requested.connect(self.seek_to_precise)

        # 时间轴
        self.timeline_area.track_remove_clicked.connect(self.remove_media)
        self.timeline_area.track_offset_changed.connect(self.set_sync_offset)
        self.timeline_area.track_move_requested.connect(tm.move_source)
        self.timeline_area.expand_requested.connect(self._on_timeline_expand_requested)

        # 分割器
        self.v_splitter.splitterMoved.connect(self._on_splitter_moved)

        # 解码器池
        dp.duration_changed.connect(self.controls_bar.set_duration)
        dp.position_changed.connect(self._on_position_changed)
        dp.frame_ready.connect(lambda: self.viewport_panel.gl_widget.update())
        dp.eof_reached.connect(self._on_eof_reached)
        dp.error_occurred.connect(self._on_decoder_error)

        # 播放控制器
        self._playback_controller.frame_tick.connect(lambda: self.viewport_panel.gl_widget.update())
        self._playback_controller.connect_to_decoder_pool()

        # 帧解码完成信号 -> 通知诊断模块
        dp.track_frame_decoded.connect(self._playback_controller.on_frame_decoded)

        # Viewport 缩放/移动
        gl_widget = self.viewport_panel.gl_widget
        gl_widget.viewport_wheel_zoom.connect(self._on_viewport_wheel_zoom)
        gl_widget.viewport_pan_start.connect(self._on_viewport_pan_start)
        gl_widget.viewport_pan_move.connect(self._on_viewport_pan_move)
        gl_widget.viewport_pan_end.connect(self._on_viewport_pan_end)
        gl_widget.viewport_resized.connect(self._on_viewport_resized)

        # ZoomComboBox -> ViewportManager
        self.controls_bar.zoom_combo.zoom_changed.connect(self._viewport_zoom_action.on_zoom_value_changed)

        # ViewportManager -> ZoomComboBox (更新 fit 值)
        self._viewport_manager.zoom_changed.connect(self._on_viewport_zoom_changed)

        # ViewportManager -> GLWidget (应用缩放/偏移渲染)
        self._viewport_manager.viewport_changed.connect(self._on_viewport_changed)

    # ========== 诊断模块设置 ==========

    def _setup_diagnostics(self):
        """设置诊断模块"""
        # 创建 StatsWindow (独立窗口)
        self._stats_window = StatsWindow()

        # 设置 DiagnosticsManager (连接所有信号)
        self._stats_window.set_diagnostics_manager(self._diagnostics_manager)

        # 性能警告
        self._diagnostics_manager.perf_monitor.performance_warning.connect(self._on_performance_warning)

        # 连接 PlaybackController 信号 -> DiagnosticsManager
        self._playback_controller.frame_requested.connect(self._diagnostics_manager.on_frame_requested)
        self._playback_controller.frame_completed.connect(self._diagnostics_manager.on_frame_completed)

    # ========== SignalBus 连接 ==========

    def _connect_signal_bus(self):
        """连接全局信号总线"""
        sb = signal_bus
        # 播放控制
        sb.play_requested.connect(self.play)
        sb.pause_requested.connect(self.pause)
        sb.seek_requested.connect(self.seek_to)
        sb.speed_changed.connect(self.controls_bar.speed_combo.setCurrentIndex)
        # zoom_changed 现在使用 set_zoom_ratio 而不是 setCurrentIndex
        sb.zoom_changed.connect(lambda ratio: self.controls_bar.zoom_combo.set_zoom_ratio(ratio / 100.0, emit=True))
        sb.loop_toggled.connect(self.controls_bar.loop_btn.setChecked)

        # 视图
        sb.view_mode_changed.connect(self.set_view_mode)
        sb.fullscreen_toggled.connect(lambda: self._action_dispatcher.dispatch("TOGGLE_FULLSCREEN"))

        # 文件
        sb.media_add_requested.connect(self.add_media)
        sb.media_remove_requested.connect(self.remove_media)

        # 调试
        sb.debug_monitor_requested.connect(self._show_memory_window)
        sb.new_window_requested.connect(self._on_new_window)

    # ========== 快捷键 ==========

    def _setup_action_system(self):
        """设置动作系统 (ActionDispatcher + Registry)"""
        # 注册所有动作
        actions = create_action_registry(self)
        self._action_dispatcher.register_batch(actions)

        # 将 ActionDispatcher 设置到 ShortcutManager
        self._shortcut_manager.set_action_dispatcher(self._action_dispatcher)

    def _setup_automation(self):
        """设置自动化控制器"""
        if not self._mock_script:
            return

        self._automation_controller = AutomationController(self._action_dispatcher, self)
        if self._automation_controller.load_script(self._mock_script):
            # 延迟启动，等待窗口完全初始化
            from PySide6.QtCore import QTimer
            QTimer.singleShot(500, self._automation_controller.start)

    def _setup_shortcuts(self):
        """设置快捷键"""
        sm = self._shortcut_manager
        sm.setup(self)

        # 传统回调方式已通过 ActionDispatcher 处理，保留此方法用于兼容
        # 如果需要使用传统方式，可以使用 sm.bind(action, callback)

    # ========== TrackManager 回调 ==========

    def _on_source_added(self, index: int, source: str):
        if self._decoder_pool.add_track(index, source):
            track_state = self._decoder_pool.get_track_state(index)
            media_info = track_state.media_info if track_state else None
            self.timeline_area.add_track(index, source, media_info)

            if self.viewport_panel.gl_widget.is_gl_initialized:
                self._decoder_pool.initialize_decoder(index)
                self.viewport_panel.gl_widget.set_decoders(self._decoder_pool.get_decoders())
                # 请求第一帧显示 (即使不播放也要显示首帧)
                self._decoder_pool.request_frame(index)

                # 更新 ViewportManager track 信息
                self._update_viewport_tracks()
        else:
            self.timeline_area.add_track(index, source)

        self.viewport_panel.add_slot(source)
        self._update_view_mode_enabled()

    def _on_source_removed(self, index: int):
        self._decoder_pool.remove_track(index)
        self.timeline_area.remove_track(index)
        self.viewport_panel.remove_slot(index)
        self.viewport_panel.gl_widget.set_decoders(self._decoder_pool.get_decoders())
        self._update_view_mode_enabled()

        # 更新 ViewportManager track 信息
        self._viewport_manager.remove_track(index)

    def _on_sources_swapped(self, index1: int, index2: int):
        self.viewport_panel.on_sources_swapped(index1, index2)
        self.timeline_area.reorder_track(index1, index2)

    def _on_sources_reordered(self, old_index: int, new_index: int):
        self.viewport_panel.on_source_moved(old_index, new_index)
        self.timeline_area.reorder_track(old_index, new_index)

    def _on_sources_reset(self):
        self._decoder_pool.clear()
        sources = self._track_manager.sources()
        self.viewport_panel.set_sources(sources)
        self.timeline_area.clear_tracks()

        for i, source in enumerate(sources):
            if self._decoder_pool.add_track(i, source):
                track_state = self._decoder_pool.get_track_state(i)
                media_info = track_state.media_info if track_state else None
                self.timeline_area.add_track(i, source, media_info)

                if self.viewport_panel.gl_widget.is_gl_initialized:
                    self._decoder_pool.initialize_decoder(i)
                    # 请求第一帧显示 (即使不播放也要显示首帧)
                    self._decoder_pool.request_frame(i)
            else:
                self.timeline_area.add_track(i, source)

        if self.viewport_panel.gl_widget.is_gl_initialized:
            self.viewport_panel.gl_widget.set_decoders(self._decoder_pool.get_decoders())

        self._update_view_mode_enabled()

    # ========== 解码器回调 ==========

    def _on_position_changed(self, position_ms: int):
        if self._decoder_pool.duration_ms > 0:
            position = position_ms / self._decoder_pool.duration_ms
            self.timeline_area.update_playhead(position)
        self.controls_bar.set_position(position_ms)

    def _on_eof_reached(self):
        self._is_playing = False
        self.controls_bar.set_playing(False)

    def _on_decoder_error(self, track_index: int, message: str):
        print(f"Decoder error (track {track_index}): {message}")

    def _toggle_stats_overlay(self):
        """切换性能统计窗口"""
        self._stats_window.toggle_window()

    def _on_performance_warning(self, track_index: int, message: str, severity: float):
        """处理性能警告"""
        from PySide6.QtWidgets import QMessageBox
        from PySide6.QtCore import Qt

        # 根据严重程度决定是否显示警告
        if severity > 0.7:  # 严重瓶颈
            # 使用非模态消息框，不阻塞播放
            msg = QMessageBox(self)
            msg.setWindowTitle("性能警告")
            msg.setText(f"<b>解码性能不足</b>")
            msg.setInformativeText(
                f"{message}\n\n"
                f"建议操作:\n"
                f"• 关闭其他占用 CPU/GPU 的程序\n"
                f"• 降低视频分辨率\n"
                f"• 禁用其他视频轨道"
            )
            msg.setIcon(QMessageBox.Icon.Warning)
            msg.setWindowFlags(msg.windowFlags() | Qt.WindowType.WindowStaysOnTopHint)
            msg.setStandardButtons(QMessageBox.StandardButton.Ok)
            msg.show()  # 非阻塞

            get_logger().warning(f"[PerfWarning] {message}")

    def _on_gl_initialized(self):
        for i in range(self._track_manager.count()):
            source = self._track_manager.get(i)
            if source:
                self._init_decoder(i, source)

        # 自动播放
        if self._auto_play and self._track_manager.count() > 0:
            self.play()

    def _init_decoder(self, index: int, source: str):
        if not self._decoder_pool.add_track(index, source):
            return
        if self.viewport_panel.gl_widget.is_gl_initialized:
            self._decoder_pool.initialize_decoder(index)
            self.viewport_panel.gl_widget.set_decoders(self._decoder_pool.get_decoders())
            # 请求第一帧显示 (即使不播放也要显示首帧)
            self._decoder_pool.request_frame(index)

            # 更新 ViewportManager track 信息
            self._update_viewport_tracks()

    # ========== Viewport 缩放/移动回调 ==========

    def _on_viewport_wheel_zoom(self, delta: int, mouse_x: float, mouse_y: float):
        """处理滚轮缩放"""
        self._viewport_zoom_action.on_wheel(delta, mouse_x, mouse_y)

    def _on_viewport_pan_start(self, x: float, y: float):
        """开始画面移动"""
        self._viewport_pan_action.on_mouse_press(x, y)

    def _on_viewport_pan_move(self, x: float, y: float):
        """画面移动中"""
        self._viewport_pan_action.on_mouse_move(x, y)

    def _on_viewport_pan_end(self):
        """结束画面移动"""
        self._viewport_pan_action.on_mouse_release()

    def _on_viewport_resized(self, width: float, height: float):
        """Viewport 尺寸变化"""
        from PySide6.QtCore import QSizeF
        self._viewport_manager.on_widget_resize(QSizeF(width, height))

    def _on_viewport_zoom_changed(self, zoom_ratio: float):
        """缩放变化回调"""
        # 更新 ZoomComboBox 显示
        self.controls_bar.set_zoom_ratio(zoom_ratio)

    def _on_viewport_changed(self):
        """Viewport 状态变化回调 - 应用到 GLWidget 渲染"""
        self.viewport_panel.gl_widget.set_viewport_transform(
            self._viewport_manager.zoom_ratio,
            self._viewport_manager.view_offset
        )

    def _update_viewport_tracks(self):
        """更新 ViewportManager 的 track 信息"""
        tracks = []
        track_sizes = [(0, 0)] * 8  # MAX_TRACKS = 8

        for i in range(self._track_manager.count()):
            track_state = self._decoder_pool.get_track_state(i)
            if track_state and track_state.media_info:
                media_info = track_state.media_info
                w = media_info.width
                h = media_info.height
                if w > 0 and h > 0:
                    tracks.append((i, w, h))
                    track_sizes[i] = (w, h)

        self._viewport_manager.set_tracks(tracks)

        # 更新 GLWidget 的 track 尺寸信息
        self.viewport_panel.gl_widget.set_track_sizes(track_sizes)

        # 更新 ZoomComboBox 的 fit 值
        if tracks:
            fit_value = self._viewport_manager.get_min_zoom()
            self.controls_bar.set_fit_value(fit_value)

    # ========== 用户操作 ==========

    def _load_initial_files(self, files: list[str]):
        for file_path in files:
            self.add_media(file_path)

    def _on_add_media(self):
        files, _ = QFileDialog.getOpenFileNames(self, "选择媒体文件", "", "所有文件 (*.*)")
        for file_path in files:
            self.add_media(file_path)

    def _on_new_window(self):
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
        subprocess.Popen([exe, script] + filtered_args)

    def dragEnterEvent(self, event: QDragEnterEvent):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent):
        for url in event.mimeData().urls():
            file_path = url.toLocalFile()
            if file_path:
                self.add_media(file_path)
        event.acceptProposedAction()

    def _update_view_mode_enabled(self):
        self.toolbar.set_view_mode_enabled(self._track_manager.count() > 0)

    def _show_memory_window(self):
        if self._memory_window is None:
            self._memory_window = MemoryWindow(
                None, auto_tracemalloc=(config.profile == Profile.DEBUG)
            )

        if self._memory_window.isMinimized():
            self._memory_window.showNormal()
        else:
            self._memory_window.show()

        self._memory_window.raise_()
        self._memory_window.activateWindow()

    def _show_settings_window(self):
        """显示设置窗口"""
        if self._settings_window is None:
            self._settings_window = SettingsWindow(self._shortcut_manager, None)

        if self._settings_window.isMinimized():
            self._settings_window.showNormal()
        else:
            self._settings_window.show()

        self._settings_window.raise_()
        self._settings_window.activateWindow()

    def _on_splitter_moved(self, _pos: int, _index: int):
        sizes = self.v_splitter.sizes()
        if len(sizes) >= 2:
            total = sum(sizes)
            if total > 0:
                max_timeline_height = int(total * 0.4)
                if sizes[1] > max_timeline_height:
                    excess = sizes[1] - max_timeline_height
                    self.v_splitter.setSizes([sizes[0] + excess, max_timeline_height])

    def _on_timeline_expand_requested(self, required_height: int):
        sizes = self.v_splitter.sizes()
        if len(sizes) >= 2:
            total = sum(sizes)
            if total > 0:
                max_timeline_height = int(total * 0.4)
                target_height = min(required_height, max_timeline_height)
                if sizes[1] < target_height:
                    self.v_splitter.setSizes([total - target_height, target_height])

    # ========== 公共 API ==========

    def add_media(self, file_path: str):
        self._track_manager.add_source(file_path)

    def remove_media(self, index: int):
        self._track_manager.remove_source(index)

    def set_sync_offset(self, index: int, offset_ms: int):
        self._decoder_pool.set_offset(index, offset_ms)

    def set_view_mode(self, mode: ViewMode):
        self._view_mode = mode
        self.viewport_panel.set_view_mode(mode)

    def play(self):
        self._is_playing = True
        self.controls_bar.set_playing(True)
        self._diagnostics_manager.start()
        self._playback_controller.play()

    def pause(self):
        self._is_playing = False
        self.controls_bar.set_playing(False)
        self._playback_controller.pause()
        self._diagnostics_manager.stop()

    def seek_to(self, timestamp_ms: int):
        """快速 seek 到关键帧"""
        get_logger().info(f"[SEEK] MainWindow.seek_to: {timestamp_ms}ms -> DecoderPool.seek_to")
        self._decoder_pool.seek_to(timestamp_ms)

    def seek_to_precise(self, timestamp_ms: int):
        """精确 seek 到目标时间之前最近的帧"""
        get_logger().info(f"[SEEK] MainWindow.seek_to_precise: {timestamp_ms}ms -> DecoderPool.seek_to_precise")
        self._decoder_pool.seek_to_precise(timestamp_ms)

    def new_project(self):
        self._track_manager.clear()

    def load_sources(self, sources: list[str]):
        self._track_manager.set_sources(sources)

    def closeEvent(self, event):
        """窗口关闭时清理资源"""
        # 1. 停止播放
        self._playback_controller.pause()
        self._diagnostics_manager.stop()

        # 2. 清理解码器 (停止所有 DecodeWorker 线程)
        self._decoder_pool.clear()

        # 3. 接受关闭事件
        event.accept()
