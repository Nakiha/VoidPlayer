"""
ControlsBar - 播放控制条
"""
from PySide6.QtWidgets import QWidget, QHBoxLayout, QLabel, QFrame
from PySide6.QtCore import Signal, Qt
from PySide6.QtGui import QColor, QFont
from qfluentwidgets import (
    TransparentToolButton,
    ComboBox,
    BodyLabel,
    Slider,
    FluentIcon,
    isDarkTheme,
    themeColor,
)

from .theme_utils import get_color_hex
from .widgets import create_tool_button


class ControlGroup(QFrame):
    """控制组控件 - 带背景的组合控件"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._update_style()

    def _update_style(self):
        """更新样式"""
        self.setStyleSheet(f"""
            ControlGroup {{
                background-color: {get_color_hex('bg_control_group')};
                border-radius: 3px;
                padding: 4px 8px;
            }}
        """)


class ControlsBar(QWidget):
    """播放控制条 - 播放控制和时间轴"""

    # 信号
    play_clicked = Signal()
    pause_clicked = Signal()
    prev_frame_clicked = Signal()
    next_frame_clicked = Signal()
    loop_toggled = Signal(bool)
    seek_requested = Signal(int)  # 毫秒
    zoom_changed = Signal(int)  # 百分比
    speed_changed = Signal(float)
    fullscreen_toggled = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._is_playing = False
        self._is_looping = True
        self._duration_ms = 9600  # 9.6秒
        self._current_ms = 0
        self.setFixedHeight(36)
        self._setup_ui()

    def _setup_ui(self):
        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 0, 8, 4)
        layout.setSpacing(4)

        # 缩放选择
        self.zoom_combo = ComboBox(self)
        self.zoom_combo.addItems(["50%", "75%", "100%", "121%", "150%", "200%", "400%"])
        self.zoom_combo.setCurrentIndex(3)  # 默认 121%
        self.zoom_combo.setFixedWidth(80)
        self.zoom_combo.currentIndexChanged.connect(self._on_zoom_changed)
        layout.addWidget(self.zoom_combo)

        # 速度选择
        self.speed_combo = ComboBox(self)
        self.speed_combo.addItems(["0.25x", "0.5x", "1x", "1.5x", "2x"])
        self.speed_combo.setCurrentIndex(2)  # 默认 1x
        self.speed_combo.setFixedWidth(70)
        self.speed_combo.currentIndexChanged.connect(self._on_speed_changed)
        layout.addWidget(self.speed_combo)

        # 全屏按钮
        self.fullscreen_btn = create_tool_button(FluentIcon.FULL_SCREEN, self, 28)
        self.fullscreen_btn.setToolTip("全屏")
        self.fullscreen_btn.clicked.connect(self.fullscreen_toggled)
        layout.addWidget(self.fullscreen_btn)

        # 上一帧按钮
        self.prev_frame_btn = create_tool_button(FluentIcon.LEFT_ARROW, self, 28)
        self.prev_frame_btn.setToolTip("上一帧")
        self.prev_frame_btn.clicked.connect(self.prev_frame_clicked)
        layout.addWidget(self.prev_frame_btn)

        # 下一帧按钮
        self.next_frame_btn = create_tool_button(FluentIcon.RIGHT_ARROW, self, 28)
        self.next_frame_btn.setToolTip("下一帧")
        self.next_frame_btn.clicked.connect(self.next_frame_clicked)
        layout.addWidget(self.next_frame_btn)

        # 循环按钮 (激发式)
        self.loop_btn = create_tool_button(FluentIcon.SYNC, self, 28)
        self.loop_btn.setToolTip("循环播放")
        self.loop_btn.setCheckable(True)
        self.loop_btn.setChecked(self._is_looping)
        self.loop_btn.clicked.connect(self._on_loop_clicked)
        self._update_loop_button_style()
        layout.addWidget(self.loop_btn)

        # 播放按钮
        self.play_btn = create_tool_button(FluentIcon.PLAY, self, 28)
        self.play_btn.setToolTip("播放")
        self.play_btn.clicked.connect(self._on_play_clicked)
        layout.addWidget(self.play_btn)

        # 时间显示
        self.time_label = BodyLabel("00:00 / 09.60", self)
        # 使用 QFont 设置字体，避免与 qfluentwidgets 主题系统冲突
        font = QFont("Consolas, Monaco, monospace")
        font.setPointSize(10)
        self.time_label.setFont(font)
        layout.addWidget(self.time_label)

        # 时间轴滑块
        self.timeline_slider = Slider(Qt.Orientation.Horizontal, self)
        self.timeline_slider.setRange(0, 1000)
        self.timeline_slider.setValue(300)  # 默认 30% 位置
        self.timeline_slider.setFixedHeight(20)  # 增加高度确保滑块完整显示
        self.timeline_slider.valueChanged.connect(self._on_slider_changed)
        layout.addWidget(self.timeline_slider, 1)

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

    def _on_loop_clicked(self):
        """循环按钮点击"""
        self._is_looping = self.loop_btn.isChecked()
        self._update_loop_button_style()
        self.loop_toggled.emit(self._is_looping)

    def _update_loop_button_style(self):
        """更新循环按钮样式"""
        # 激发状态下使用主题色背景高亮
        if self._is_looping:
            accent_color = themeColor()
            # 使用 rgba 格式设置半透明背景色
            r, g, b = accent_color.red(), accent_color.green(), accent_color.blue()
            # 设置与按钮大小一致的背景区域
            self.loop_btn.setStyleSheet(f"""
                TransparentToolButton {{
                    background-color: rgba({r}, {g}, {b}, 60);
                    border: none;
                    border-radius: 14px;
                    padding: 0px;
                    margin: 0px;
                }}
                TransparentToolButton:hover {{
                    background-color: rgba({r}, {g}, {b}, 100);
                    border-radius: 14px;
                }}
                TransparentToolButton:pressed {{
                    background-color: rgba({r}, {g}, {b}, 140);
                    border-radius: 14px;
                }}
            """)
        else:
            self.loop_btn.setStyleSheet("""
                TransparentToolButton {
                    background-color: transparent;
                    border: none;
                    border-radius: 14px;
                    padding: 0px;
                    margin: 0px;
                }
                TransparentToolButton:hover {
                    background-color: rgba(255, 255, 255, 20);
                    border-radius: 14px;
                }
                TransparentToolButton:pressed {
                    background-color: rgba(255, 255, 255, 40);
                    border-radius: 14px;
                }
            """)

    def _on_slider_changed(self, value: int):
        """滑块值变化"""
        self._current_ms = int(value / 1000 * self._duration_ms)
        self._update_time_display()
        self.seek_requested.emit(self._current_ms)

    def _on_zoom_changed(self, index: int):
        """缩放变化"""
        zoom_values = [50, 75, 100, 121, 150, 200, 400]
        self.zoom_changed.emit(zoom_values[index])

    def _on_speed_changed(self, index: int):
        """速度变化"""
        speed_values = [0.25, 0.5, 1.0, 1.5, 2.0]
        self.speed_changed.emit(speed_values[index])

    def _update_time_display(self):
        """更新时间显示"""
        current_sec = self._current_ms / 1000
        total_sec = self._duration_ms / 1000
        self.time_label.setText(f"{current_sec:05.2f} / {total_sec:05.2f}")

    def set_duration(self, duration_ms: int):
        """设置总时长"""
        self._duration_ms = duration_ms
        self._update_time_display()

    def set_current_time(self, time_ms: int):
        """设置当前时间"""
        self._current_ms = time_ms
        # 更新滑块 (不触发信号)
        self.timeline_slider.blockSignals(True)
        if self._duration_ms > 0:
            self.timeline_slider.setValue(int(time_ms / self._duration_ms * 1000))
        self.timeline_slider.blockSignals(False)
        self._update_time_display()

    def set_playing(self, is_playing: bool):
        """设置播放状态"""
        self._is_playing = is_playing
        self.play_btn.setIcon(FluentIcon.PAUSE if is_playing else FluentIcon.PLAY)
