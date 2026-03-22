"""
ControlsBar - 播放控制条
"""
import time
from PySide6.QtWidgets import QWidget, QHBoxLayout
from PySide6.QtCore import Signal, Qt
from qfluentwidgets_nuitka import (
    ComboBox,
    FluentIcon,
    ToolTipFilter,
    TransparentToggleToolButton,
)

from player.core.logging_config import get_logger

from .theme_utils import get_color_hex, ColorKey
from .widgets import create_tool_button, TimeLabel, TimelineSlider, ZoomComboBox


class ControlsBar(QWidget):
    """播放控制条 - 播放控制和时间轴"""

    # 信号
    play_clicked = Signal()
    pause_clicked = Signal()
    prev_frame_clicked = Signal()
    next_frame_clicked = Signal()
    loop_toggled = Signal(bool)
    seek_requested = Signal(int)  # 毫秒，快速 seek (keyframe)
    precise_seek_requested = Signal(int)  # 毫秒，精确 seek (frame-accurate)
    zoom_changed = Signal(int)  # 百分比
    speed_changed = Signal(float)
    fullscreen_toggled = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._is_playing = False
        self._is_looping = True
        self._duration_ms = 0  # 总时长，由外部设置
        self._current_ms = 0
        self._logger = get_logger()
        self.setFixedHeight(40)  # 32px 按钮 + 4px*2 边距 = 40px
        self._update_style()
        self._setup_ui()

    def _update_style(self):
        """更新背景样式 - 稍亮引导用户操作"""
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setStyleSheet(f"""
            ControlsBar {{
                background-color: {get_color_hex(ColorKey.BG_CONTROL_GROUP)};
            }}
        """)

    def _setup_ui(self):
        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(4)

        # 缩放选择 (使用自定义 ZoomComboBox)
        self.zoom_combo = ZoomComboBox(self)
        self.zoom_combo.setFocusPolicy(Qt.FocusPolicy.ClickFocus)  # 允许编辑时获取焦点
        self.zoom_combo.zoom_changed.connect(self._on_zoom_changed)
        layout.addWidget(self.zoom_combo)

        # 速度选择
        self.speed_combo = ComboBox(self)
        self.speed_combo.addItems(["0.25x", "0.5x", "1x", "1.5x", "2x"])
        self.speed_combo.setCurrentIndex(2)  # 默认 1x
        self.speed_combo.setFixedWidth(70)
        self.speed_combo.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # 不拦截快捷键
        self.speed_combo.currentIndexChanged.connect(self._on_speed_changed)
        layout.addWidget(self.speed_combo)

        # 全屏按钮
        self.fullscreen_btn = create_tool_button(FluentIcon.FULL_SCREEN, self, 32, "全屏")
        self.fullscreen_btn.clicked.connect(self.fullscreen_toggled)
        layout.addWidget(self.fullscreen_btn)

        # 上一帧按钮
        self.prev_frame_btn = create_tool_button(FluentIcon.LEFT_ARROW, self, 32, "上一帧")
        self.prev_frame_btn.clicked.connect(self.prev_frame_clicked)
        layout.addWidget(self.prev_frame_btn)

        # 下一帧按钮
        self.next_frame_btn = create_tool_button(FluentIcon.RIGHT_ARROW, self, 32, "下一帧")
        self.next_frame_btn.clicked.connect(self.next_frame_clicked)
        layout.addWidget(self.next_frame_btn)

        # 循环按钮 (激发式)
        self.loop_btn = TransparentToggleToolButton(self)
        self.loop_btn.setIcon(FluentIcon.SYNC)
        self.loop_btn.setFixedSize(32, 32)
        self.loop_btn.setToolTip("循环播放")
        self.loop_btn.installEventFilter(ToolTipFilter(self.loop_btn, 0))
        self.loop_btn.setChecked(self._is_looping)
        self.loop_btn.toggled.connect(self._on_loop_toggled)
        layout.addWidget(self.loop_btn)

        # 播放按钮
        self.play_btn = create_tool_button(FluentIcon.PLAY, self, 32, "播放")
        self.play_btn.clicked.connect(self._on_play_clicked)
        layout.addWidget(self.play_btn)

        # 时间显示
        self.time_label = TimeLabel(self)
        layout.addWidget(self.time_label)

        # 时间轴进度条
        self.timeline_slider = TimelineSlider(self)
        self.timeline_slider.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # 不拦截快捷键
        self.timeline_slider.position_dragging.connect(self._on_slider_dragging)
        self.timeline_slider.position_changed.connect(self._on_slider_changed)
        layout.addWidget(self.timeline_slider, 1)

        # 初始化控件状态
        self._update_controls_enabled()
        self._update_time_display()

    def _update_controls_enabled(self):
        """根据是否加载媒体来启用/禁用控件"""
        has_media = self._duration_ms > 0

        # 媒体播放相关控件
        self.zoom_combo.setEnabled(has_media)
        self.speed_combo.setEnabled(has_media)
        self.fullscreen_btn.setEnabled(has_media)
        self.prev_frame_btn.setEnabled(has_media)
        self.next_frame_btn.setEnabled(has_media)
        self.loop_btn.setEnabled(has_media)
        self.play_btn.setEnabled(has_media)
        self.timeline_slider.setEnabled(has_media)

    def _on_play_clicked(self):
        """播放按钮点击"""
        if self._is_playing:
            self._is_playing = False
            self.play_btn.setIcon(FluentIcon.PLAY)
            self.pause_clicked.emit()
        else:
            self._is_playing = True
            self.play_btn.setIcon(FluentIcon.PAUSE)
            self.play_clicked.emit()

    def _on_loop_toggled(self, checked: bool):
        """循环按钮状态切换"""
        self._is_looping = checked
        self.loop_toggled.emit(checked)

    def _on_slider_dragging(self, position_us: int):
        """滑块拖动中 - 更新时间显示，发送快速 seek"""
        t0 = time.perf_counter()
        self._current_ms = position_us // 1000  # 微秒转毫秒
        self._update_time_display()
        self._logger.info(f"[SEEK] ControlsBar._on_slider_dragging: {self._current_ms}ms, emit seek_requested")
        self.seek_requested.emit(self._current_ms)
        self._logger.info(f"[SEEK] ControlsBar._on_slider_dragging done: {(time.perf_counter() - t0)*1000:.2f}ms")

    def _on_slider_changed(self, position_us: int):
        """滑块位置最终确定 - 发送精确 seek"""
        t0 = time.perf_counter()
        self._current_ms = position_us // 1000  # 微秒转毫秒
        self._update_time_display()
        self._logger.info(f"[SEEK] ControlsBar._on_slider_changed: {self._current_ms}ms, emit precise_seek_requested")
        self.precise_seek_requested.emit(self._current_ms)
        self._logger.info(f"[SEEK] ControlsBar._on_slider_changed done: {(time.perf_counter() - t0)*1000:.2f}ms")

    def _on_zoom_changed(self, zoom_ratio: float):
        """缩放变化 - zoom_ratio 是比例值 (1.0 = 100%)"""
        # 转换为百分比发送给外部
        self.zoom_changed.emit(int(zoom_ratio * 100))

    def set_zoom_ratio(self, ratio: float):
        """设置缩放比例

        Args:
            ratio: 缩放比例 (1.0 = 100%)
        """
        self.zoom_combo.set_zoom_ratio(ratio, emit=False)

    def set_fit_value(self, fit_value: float):
        """设置 fit 值

        Args:
            fit_value: fit 缩放比例
        """
        self.zoom_combo.set_fit_value(fit_value)

    def _on_speed_changed(self, index: int):
        """速度变化"""
        speed_values = [0.25, 0.5, 1.0, 1.5, 2.0]
        self.speed_changed.emit(speed_values[index])

    def _update_time_display(self):
        """更新时间显示"""
        self.time_label.setTime(self._current_ms, self._duration_ms)

    def set_duration(self, duration_ms: int):
        """设置总时长"""
        self._duration_ms = duration_ms
        self.timeline_slider.set_duration(duration_ms * 1000)  # 毫秒转微秒
        self._update_controls_enabled()
        self._update_time_display()

    def set_current_time(self, time_ms: int):
        """设置当前时间"""
        self._current_ms = time_ms
        self.timeline_slider.set_position(time_ms * 1000)  # 毫秒转微秒
        self._update_time_display()

    def set_position(self, position_ms: int):
        """设置当前播放位置 (别名)"""
        self.set_current_time(position_ms)

    def set_playing(self, is_playing: bool):
        """设置播放状态"""
        self._is_playing = is_playing
        self.play_btn.setIcon(FluentIcon.PAUSE if is_playing else FluentIcon.PLAY)
