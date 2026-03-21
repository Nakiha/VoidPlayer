"""
时间相关控件和工具函数
"""
from PySide6.QtGui import QFont, QFontMetrics
from PySide6.QtWidgets import QSizePolicy
from qfluentwidgets_nuitka import BodyLabel


# ========== 工具函数 ==========

def format_time_seconds(seconds: float) -> str:
    """将秒数格式化为 SS.CC 格式 (秒.百分秒)"""
    abs_sec = abs(seconds)
    sec = int(abs_sec)
    centisec = int((abs_sec - sec) * 100)
    return f"{sec:02d}.{centisec:02d}"


def format_time_ms(ms: int, show_sign: bool = False) -> str:
    """
    将毫秒格式化为时间字符串

    格式规则:
    - 始终显示3位小数: SS.SSS
    - 超过1分钟: MM:SS.SSS
    - 超过1小时: H:MM:SS.SSS (小时数可以超过24)

    Args:
        ms: 毫秒值
        show_sign: 是否显示正负号 (+/-)
    """
    # 确定符号
    if ms < 0:
        sign = "-"
        abs_ms = abs(ms)
    elif show_sign and ms > 0:
        sign = "+"
        abs_ms = ms
    else:
        sign = ""
        abs_ms = ms

    # 计算各部分
    total_seconds = abs_ms // 1000
    milliseconds = abs_ms % 1000
    hours = total_seconds // 3600
    minutes = (total_seconds % 3600) // 60
    seconds = total_seconds % 60

    ms_str = f".{milliseconds:03d}"

    if hours > 0:
        # 超过1小时: H:MM:SS.SSS
        return f"{sign}{hours}:{minutes:02d}:{seconds:02d}{ms_str}"
    elif minutes > 0:
        # 超过1分钟: MM:SS.SSS
        return f"{sign}{minutes:02d}:{seconds:02d}{ms_str}"
    else:
        # 不足1分钟: SS.SSS
        return f"{sign}{seconds:02d}{ms_str}"


# ========== 控件 ==========

class TimeLabel(BodyLabel):
    """统一的播放时间显示控件 - 显示 当前/总时长 格式"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._current_ms = 0
        self._duration_ms = 0
        self._apply_style()
        self._update_display()

    def _apply_style(self):
        """应用等宽字体样式"""
        font = QFont("Consolas, Monaco, monospace")
        font.setPointSize(10)
        self.setFont(font)
        # 透明背景，避免遮挡父控件背景
        self.setStyleSheet("TimeLabel { background: transparent; }")

    def setTime(self, current_ms: int, duration_ms: int):
        """设置当前时间和总时长"""
        self._current_ms = current_ms
        self._duration_ms = duration_ms
        self._update_display()

    def setCurrentTime(self, ms: int):
        """设置当前时间"""
        self._current_ms = ms
        self._update_display()

    def setDuration(self, ms: int):
        """设置总时长"""
        self._duration_ms = ms
        self._update_display()

    def _update_display(self):
        """更新显示"""
        if self._duration_ms <= 0:
            self.setText("--:-- / --:--")
        else:
            current_str = format_time_ms(self._current_ms)
            duration_str = format_time_ms(self._duration_ms)
            self.setText(f"{current_str} / {duration_str}")


class OffsetLabel(BodyLabel):
    """偏移时间显示控件 - 显示带符号的偏移值"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._offset_ms = 0
        self._apply_style()
        self._update_display()

    def _apply_style(self):
        """应用等宽字体样式"""
        font = QFont("Consolas, Monaco, monospace")
        font.setPointSize(10)
        self.setFont(font)
        # 透明背景，避免遮挡父控件背景
        self.setStyleSheet("OffsetLabel { background: transparent; }")
        # 最小宽度刚好包裹 "00:00"
        fm = QFontMetrics(font)
        min_width = fm.horizontalAdvance("00:00") + 4  # 加一点边距
        self.setMinimumWidth(min_width)
        # 允许根据内容自动扩展宽度
        self.setSizePolicy(QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Preferred)

    def setOffset(self, ms: int):
        """设置偏移值"""
        self._offset_ms = ms
        self._update_display()

    def offset(self) -> int:
        """获取当前偏移值"""
        return self._offset_ms

    def _update_display(self):
        """更新显示"""
        self.setText(format_time_ms(self._offset_ms, show_sign=True))
        self.updateGeometry()  # 通知布局重新计算大小
