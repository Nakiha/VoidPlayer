"""
VoidPlayer 控件模块

包含播放器使用的各种自定义控件和工具函数。

模块结构:
- time_widgets: 时间相关控件和工具函数 (TimeLabel, OffsetLabel, format_time_*)
- buttons: 按钮工具函数 (create_tool_button)
- highlight_splitter: 悬浮高亮分割器 (HighlightSplitter)
- resizable_container: 可拖动调整大小的容器 (ResizableContainer)
- timeline_slider: 时间轴进度条 (TimelineSlider)
"""

# 时间控件和工具函数
from .time_widgets import format_time_seconds, format_time_ms, TimeLabel, OffsetLabel

# 按钮工具
from .buttons import create_tool_button

# 悬浮高亮分割器
from .highlight_splitter import HighlightSplitter

# 可调整大小容器
from .resizable_container import ResizableContainer

# 省略文本标签
from .elide_label import ElideLabel

# 可拖拽省略文本标签
from .draggable_label import DraggableElideLabel

# 省略文本下拉框
from .elide_combo import ElideComboBox

# 时间轴进度条
from .timeline_slider import TimelineSlider

__all__ = [
    # 时间工具
    'format_time_seconds',
    'format_time_ms',
    # 时间控件
    'TimeLabel',
    'OffsetLabel',
    # 按钮工具
    'create_tool_button',
    # 悬浮高亮分割器
    'HighlightSplitter',
    # 可调整大小容器
    'ResizableContainer',
    # 省略文本标签
    'ElideLabel',
    # 可拖拽省略文本标签
    'DraggableElideLabel',
    # 省略文本下拉框
    'ElideComboBox',
    # 时间轴进度条
    'TimelineSlider',
]
