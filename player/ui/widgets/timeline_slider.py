"""
TimelineSlider - 时间轴进度条控件
WinUI 3 风格的播放器进度条，支持微秒精度
"""
from PySide6.QtWidgets import QWidget
from PySide6.QtCore import Signal, Qt, QRect, QRectF, Property, QPropertyAnimation, QEasingCurve
from PySide6.QtGui import (
    QPainter,
    QColor,
    QPen,
    QBrush,
    QFont,
    QPainterPath,
    QFontMetrics,
)

from ..theme_utils import get_color, ColorKey, get_accent_color


# 拖动检测阈值 (像素)
DRAG_THRESHOLD = 4


class _TooltipOverlay(QWidget):
    """Tooltip 覆盖层 - 放置在顶层窗口上，可超出 TimelineSlider 边界绘制"""

    # 常量
    TOOLTIP_HEIGHT = 22
    TOOLTIP_PADDING = 8
    TRIANGLE_SIZE = 6
    TOOLTIP_OFFSET = 4  # tooltip 与轨道的间距

    def __init__(self, slider: QWidget):
        super().__init__()  # 无父控件，独立窗口
        self._slider = slider
        self._hover_x = 0  # 相对于 slider 的 x 坐标
        self._time_text = ""
        self.setWindowFlags(
            Qt.WindowType.Tool |
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.NoDropShadowWindowHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.hide()

    def update_tooltip(self, hover_x: int, time_text: str):
        """更新 tooltip 内容"""
        self._hover_x = hover_x
        self._time_text = time_text
        self._update_geometry()
        self.show()
        self.update()

    def hide_tooltip(self):
        """隐藏 tooltip"""
        self.hide()

    def _update_geometry(self):
        """更新 overlay 的几何位置 - 使用屏幕坐标"""
        if not self._slider:
            return
        # slider 在屏幕上的位置
        slider_pos = self._slider.mapToGlobal(self._slider.rect().topLeft())
        slider_width = self._slider.width()
        # tooltip 在 slider 上方的位置 (屏幕坐标)
        tooltip_y = slider_pos.y() - self.TOOLTIP_HEIGHT - self.TRIANGLE_SIZE - self.TOOLTIP_OFFSET
        # overlay 位置：与 slider 对齐，高度为 tooltip + 三角 + 偏移
        total_height = self.TOOLTIP_HEIGHT + self.TRIANGLE_SIZE + self.TOOLTIP_OFFSET
        overlay_rect = QRect(
            slider_pos.x(),
            tooltip_y,
            slider_width,
            total_height
        )
        self.setGeometry(overlay_rect)

    def paintEvent(self, event):
        if not self._time_text:
            return

        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        # 主题色作为背景
        accent = get_accent_color()
        painter.setBrush(QBrush(accent))
        painter.setPen(Qt.PenStyle.NoPen)

        # 设置字体
        font = QFont("Segoe UI", 9)
        painter.setFont(font)
        fm = QFontMetrics(font)
        text_width = fm.horizontalAdvance(self._time_text)
        tooltip_width = text_width + self.TOOLTIP_PADDING * 2
        tooltip_height = self.TOOLTIP_HEIGHT

        # 计算 tooltip 位置 (水平居中于鼠标)
        tooltip_x = self._hover_x - tooltip_width / 2
        # 限制不超出 overlay 边界
        tooltip_x = max(0, min(tooltip_x, self.width() - tooltip_width))
        # tooltip 在 overlay 顶部
        tooltip_y = 0

        # 绘制 tooltip 背景 (圆角矩形)
        tooltip_rect = QRectF(tooltip_x, tooltip_y, tooltip_width, tooltip_height)
        painter.drawRoundedRect(tooltip_rect, 4, 4)

        # 绘制倒三角 (指向鼠标位置，在 tooltip 底部)
        tri_top = tooltip_height + self.TRIANGLE_SIZE
        tri_bottom = tooltip_height
        half_size = self.TRIANGLE_SIZE / 2

        triangle_path = QPainterPath()
        triangle_path.moveTo(self._hover_x, tri_top)  # 顶点 (朝下)
        triangle_path.lineTo(self._hover_x - half_size, tri_bottom)  # 左上
        triangle_path.lineTo(self._hover_x + half_size, tri_bottom)  # 右上
        triangle_path.closeSubpath()
        painter.drawPath(triangle_path)

        # 绘制文字 - 根据主题色亮度选择对比色
        # 计算亮度 (ITU-R BT.601 标准)
        luminance = 0.299 * accent.red() + 0.587 * accent.green() + 0.114 * accent.blue()
        text_color = QColor(0, 0, 0) if luminance > 128 else QColor(255, 255, 255)
        painter.setPen(QPen(text_color))
        text_x = tooltip_x + (tooltip_width - text_width) / 2
        text_y = tooltip_y + (tooltip_height + fm.ascent() - fm.descent()) / 2
        painter.drawText(int(text_x), int(text_y), self._time_text)


class TimelineSlider(QWidget):
    """
    时间轴进度条控件

    特性:
    - 微秒精度 (int)
    - 6px 轨道高度，无滑块手柄，轨道居中且两端圆角
    - 悬停时显示主题色 tooltip + 倒三角标记
    - 支持缓冲进度显示
    - 区分点击和拖动：点击用精确 seek，拖动用快速 seek
    """

    # 信号
    position_dragging = Signal(int)   # 拖动中持续发送 (微秒)，用于实时预览
    position_changed = Signal(int)    # 最终位置确定时发送 (微秒)，用于精确 seek

    # 常量
    TRACK_HEIGHT = 6  # 轨道高度
    TRACK_RADIUS = 3  # 轨道圆角半径

    def __init__(self, parent=None):
        super().__init__(parent)
        self._duration = 0  # 微秒
        self._position = 0  # 微秒
        self._buffer_position = 0  # 缓冲位置 (微秒)

        # 交互状态
        self._hovering = False
        self._hover_x = 0
        self._press_x = 0  # 按下时的 x 坐标，用于判断是否是拖动
        self._is_dragging = False  # 是否真正在拖动（超过阈值）
        self._pressed = False  # 鼠标是否按下

        # Tooltip overlay
        self._tooltip = _TooltipOverlay(self)

        # 启用鼠标追踪以支持悬停效果
        self.setMouseTracking(True)
        self.setFixedHeight(self.TRACK_HEIGHT)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    # ========== 公共 API ==========

    def set_duration(self, duration_us: int) -> None:
        """设置总时长 (微秒)"""
        self._duration = max(0, duration_us)
        self.update()

    def duration(self) -> int:
        """获取总时长 (微秒)"""
        return self._duration

    def set_position(self, position_us: int) -> None:
        """设置当前位置 (微秒)，不触发信号"""
        self._position = max(0, min(position_us, self._duration))
        self.update()

    def position(self) -> int:
        """获取当前位置 (微秒)"""
        return self._position

    def set_buffer_position(self, position_us: int) -> None:
        """设置缓冲位置 (微秒)"""
        self._buffer_position = max(0, min(position_us, self._duration))
        self.update()

    def buffer_position(self) -> int:
        """获取缓冲位置 (微秒)"""
        return self._buffer_position

    # ========== 内部计算 ==========

    def _track_rect(self) -> QRectF:
        """获取轨道矩形 (垂直居中)"""
        y = (self.height() - self.TRACK_HEIGHT) / 2
        return QRectF(0, y, self.width(), self.TRACK_HEIGHT)

    def _x_to_position(self, x: int) -> int:
        """X 坐标转微秒位置"""
        if self.width() <= 0 or self._duration <= 0:
            return 0
        ratio = x / self.width()
        ratio = max(0, min(1, ratio))
        return int(ratio * self._duration)

    def _position_to_x(self, position_us: int) -> int:
        """微秒位置转 X 坐标"""
        if self._duration <= 0:
            return 0
        ratio = position_us / self._duration
        return int(ratio * self.width())

    def _format_time(self, us: int) -> str:
        """格式化微秒为 MM:SS.ss"""
        total_seconds = us / 1_000_000
        minutes = int(total_seconds // 60)
        seconds = total_seconds % 60
        return f"{minutes:02d}:{seconds:05.2f}"

    # ========== 事件处理 ==========

    def enterEvent(self, event):
        self._hovering = True
        self._update_tooltip()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self._hovering = False
        self._is_dragging = False
        self._pressed = False
        self._tooltip.hide_tooltip()
        self.update()
        super().leaveEvent(event)

    def mouseMoveEvent(self, event):
        self._hover_x = int(event.position().x())
        if self._pressed:
            # 检查是否超过拖动阈值
            if not self._is_dragging:
                if abs(self._hover_x - self._press_x) > DRAG_THRESHOLD:
                    self._is_dragging = True

            if self._is_dragging:
                pos_us = self._x_to_position(self._hover_x)
                self._position = pos_us
                self.position_dragging.emit(pos_us)

        self._update_tooltip()
        self.update()
        super().mouseMoveEvent(event)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._pressed = True
            self._is_dragging = False
            self._press_x = int(event.position().x())
            self._hover_x = self._press_x
            pos_us = self._x_to_position(self._hover_x)
            self._position = pos_us
            self._update_tooltip()
            self.update()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self._pressed:
            self._pressed = False
            self._hover_x = int(event.position().x())
            pos_us = self._x_to_position(self._hover_x)
            self._position = pos_us
            # 无论是否拖动，都在释放时发送最终位置
            self.position_changed.emit(pos_us)
            self._is_dragging = False
            self._update_tooltip()
            self.update()

    def _update_tooltip(self):
        """更新 tooltip 显示"""
        if self._hovering and self._duration > 0:
            hover_pos_us = self._x_to_position(self._hover_x)
            time_text = self._format_time(hover_pos_us)
            self._tooltip.update_tooltip(self._hover_x, time_text)
        else:
            self._tooltip.hide_tooltip()

    # ========== 绘制 ==========

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        track = self._track_rect()

        # 1. 绘制轨道背景 (圆角)
        bg_color = get_color(ColorKey.BG_TRACK_ALT)
        painter.setBrush(QBrush(bg_color))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(track, self.TRACK_RADIUS, self.TRACK_RADIUS)

        # 2. 绘制缓冲进度 (圆角)
        if self._buffer_position > 0:
            buffer_x = self._position_to_x(self._buffer_position)
            if buffer_x > 0:
                buffer_color = get_color(ColorKey.TEXT_SECONDARY)
                buffer_color.setAlpha(128)
                painter.setBrush(QBrush(buffer_color))
                painter.setClipRect(QRectF(0, track.top(), buffer_x, track.height()))
                painter.drawRoundedRect(track, self.TRACK_RADIUS, self.TRACK_RADIUS)
                painter.setClipping(False)

        # 3. 绘制播放进度 (主题色，圆角)
        if self._position > 0:
            pos_x = self._position_to_x(self._position)
            if pos_x > 0:
                accent = get_accent_color()
                painter.setBrush(QBrush(accent))
                painter.setClipRect(QRectF(0, track.top(), pos_x, track.height()))
                painter.drawRoundedRect(track, self.TRACK_RADIUS, self.TRACK_RADIUS)
                painter.setClipping(False)
