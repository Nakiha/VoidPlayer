"""
时间相关控件和工具函数
"""
import re
from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QFont, QFontMetrics
from PySide6.QtWidgets import QHBoxLayout, QWidget
from qfluentwidgets_nuitka import BodyLabel, LineEdit


# ========== 基础控件 ==========

class AutoWidthLineEdit(LineEdit):
    """根据文本内容自动调整宽度的 LineEdit"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._min_width = 0
        self._max_width = 16777215  # QWIDGETSIZE_MAX
        self.textChanged.connect(self._adjust_width)

    def setWidthRange(self, min_chars: int, max_chars: int):
        """设置宽度范围（字符数）"""
        fm = QFontMetrics(self.font())
        self._min_width = fm.horizontalAdvance("0" * min_chars) + 4
        self._max_width = fm.horizontalAdvance("0" * max_chars) + 4
        self._adjust_width()

    def _adjust_width(self):
        """根据文本内容调整宽度"""
        fm = QFontMetrics(self.font())
        text_width = fm.horizontalAdvance(self.text()) + 8  # 加一点边距
        new_width = max(self._min_width, min(text_width, self._max_width))
        self.setFixedWidth(int(new_width))


# ========== 工具函数 ==========

def format_time_seconds(seconds: float) -> str:
    """将秒数格式化为 SS.CC 格式 (秒.百分秒)"""
    abs_sec = abs(seconds)
    sec = int(abs_sec)
    centisec = int((abs_sec - sec) * 100)
    return f"{sec:02d}.{centisec:02d}"


def parse_time_ms(time_str: str) -> int | None:
    """
    将时间字符串解析为毫秒

    支持格式:
    - SS.SSS 或 SS.S 或 SS
    - MM:SS.SSS 或 MM:SS.S 或 MM:SS
    - H:MM:SS.SSS 或 H:MM:SS.S 或 H:MM:SS

    Args:
        time_str: 时间字符串

    Returns:
        毫秒值，解析失败返回 None
    """
    time_str = time_str.strip()

    # 匹配三种格式: H:MM:SS.SSS, MM:SS.SSS, SS.SSS
    pattern = r'^(?:(\d+):)?(\d{1,2})(?::(\d{2}))?(?:\.(\d{1,3}))?$'
    match = re.match(pattern, time_str)
    if not match:
        return None

    groups = match.groups()

    # 判断格式
    if groups[2] is not None:
        # H:MM:SS.SSS 格式
        hours = int(groups[0] or 0)
        minutes = int(groups[1])
        seconds = int(groups[2])
        ms_str = groups[3] or '0'
    elif groups[0] is not None:
        # MM:SS.SSS 格式
        hours = 0
        minutes = int(groups[0])
        seconds = int(groups[1])
        ms_str = groups[3] or '0'
    else:
        # SS.SSS 格式
        hours = 0
        minutes = 0
        seconds = int(groups[1])
        ms_str = groups[3] or '0'

    # 解析毫秒部分（补齐到3位）
    ms_str = (ms_str + '000')[:3]
    milliseconds = int(ms_str)

    # 验证范围
    if minutes >= 60 or seconds >= 60:
        return None

    total_ms = hours * 3600000 + minutes * 60000 + seconds * 1000 + milliseconds
    return total_ms


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

class TimeLabel(QWidget):
    """统一的播放时间显示控件 - 显示 当前/总时长 格式，当前时间可编辑"""

    # 用户编辑完成信号，参数为毫秒值
    time_edit_finished = Signal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._current_ms = 0
        self._duration_ms = 0
        self._setup_ui()
        self._connect_signals()
        self._update_display()

    def _setup_ui(self):
        """设置UI布局"""
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        # 等宽字体
        font = QFont("Consolas, Monaco, monospace")
        font.setPointSize(10)

        # 当前时间（可编辑，自适应宽度）
        self._current_edit = AutoWidthLineEdit(self)
        self._current_edit.setFont(font)
        self._current_edit.setReadOnly(False)
        self._current_edit.setClearButtonEnabled(False)
        self._current_edit.setWidthRange(5, 9)  # 最短5字符，最长9字符
        self._current_edit.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self._current_edit.setStyleSheet("LineEdit { background: transparent; border: none; }")

        # 分隔符
        self._sep_label = BodyLabel(" / ", self)
        self._sep_label.setFont(font)
        self._sep_label.setStyleSheet("BodyLabel { background: transparent; }")

        # 总时长（只读显示）
        self._duration_label = BodyLabel(self)
        self._duration_label.setFont(font)
        self._duration_label.setStyleSheet("BodyLabel { background: transparent; }")

        layout.addWidget(self._current_edit)
        layout.addWidget(self._sep_label)
        layout.addWidget(self._duration_label)

        # 透明背景
        self.setStyleSheet("TimeLabel { background-color: transparent; }")

    def _connect_signals(self):
        """连接信号"""
        # 编辑完成时处理（失去焦点或按下 Enter）
        self._current_edit.editingFinished.connect(self._on_editing_finished)

    def _on_editing_finished(self):
        """编辑完成处理"""
        text = self._current_edit.text().strip()
        if not text or text == "--:--":
            return

        parsed_ms = parse_time_ms(text)
        if parsed_ms is not None:
            # 限制在有效范围内
            parsed_ms = max(0, min(parsed_ms, self._duration_ms))
            if parsed_ms != self._current_ms:
                self._current_ms = parsed_ms
                self.time_edit_finished.emit(parsed_ms)
            # 无论是否变化，都重新格式化显示
            self._current_edit.setText(format_time_ms(self._current_ms))
        else:
            # 解析失败，恢复原值
            self._current_edit.setText(format_time_ms(self._current_ms))

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

    def currentEdit(self) -> LineEdit:
        """获取当前时间编辑框"""
        return self._current_edit

    def _update_display(self):
        """更新显示"""
        if self._duration_ms <= 0:
            self._current_edit.setText("--:--")
            self._duration_label.setText("--:--")
        else:
            current_str = format_time_ms(self._current_ms)
            duration_str = format_time_ms(self._duration_ms)
            self._current_edit.setText(current_str)
            self._duration_label.setText(duration_str)


class OffsetLabel(AutoWidthLineEdit):
    """偏移时间显示控件 - 显示带符号的偏移值，可编辑"""

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
        # 透明背景，无边框
        self.setStyleSheet("OffsetLabel { background: transparent; border: none; }")
        # 宽度范围：最短5字符，最长10字符（带符号如 "+00:00.000"）
        self.setWidthRange(5, 10)
        # 右对齐
        self.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self.setClearButtonEnabled(False)

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
