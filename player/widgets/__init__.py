"""
VoidPlayer 控件模块

包含播放器使用的各种自定义控件和工具函数。

模块结构:
- time_utils: 时间格式化工具函数
- labels: 时间标签控件 (TimeLabel, OffsetLabel)
- buttons: 按钮工具函数 (create_tool_button)
- resize: 拖动调整控件 (ResizableContainer)
"""

# 时间工具函数
from .time_utils import format_time_seconds, format_time_ms

# 标签控件
from .labels import TimeLabel, OffsetLabel

# 按钮工具
from .buttons import create_tool_button

# 拖动调整控件
from .resize import ResizableContainer

__all__ = [
    # 时间工具
    'format_time_seconds',
    'format_time_ms',
    # 标签控件
    'TimeLabel',
    'OffsetLabel',
    # 按钮工具
    'create_tool_button',
    # 拖动调整控件
    'ResizableContainer',
]
