"""
ViewMode - 视图模式枚举定义
"""
from enum import Enum


class ViewMode(Enum):
    """视图模式枚举"""
    SIDE_BY_SIDE = 0  # 并排模式 - 左右各占 50%，固定中线
    SPLIT_SCREEN = 1  # 分屏模式 - 可拖动分割线位置
