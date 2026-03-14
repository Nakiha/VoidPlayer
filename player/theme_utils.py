"""
ThemeUtils - 主题工具模块
动态适配亮色/暗色主题的颜色系统
"""
from enum import StrEnum

from qfluentwidgets import isDarkTheme, themeColor
from PySide6.QtGui import QColor


class ColorKey(StrEnum):
    """颜色键枚举 - 所有颜色必须通过此枚举访问"""

    # 背景色
    BG_BASE = "bg_base"
    BG_TIMELINE = "bg_timeline"
    BG_TRACK_CONTROLS = "bg_track_controls"
    BG_TRACK_ALT = "bg_track_alt"
    BG_CONTROL_GROUP = "bg_control_group"
    BG_CLIP = "bg_clip"

    # 文字色
    TEXT_PRIMARY = "text_primary"
    TEXT_SECONDARY = "text_secondary"

    # Chart 图表色
    CHART_BG = "chart_bg"
    CHART_GRID = "chart_grid"
    CHART_LINE = "chart_line"
    CHART_LINE_SECONDARY = "chart_line_secondary"
    CHART_TEXT = "chart_text"


# 颜色定义：支持亮/暗主题自动切换
# 格式: (暗色值, 亮色值) 或 None 表示固有色
_COLOR_DEFINITIONS: dict[ColorKey, QColor | tuple[QColor, QColor]] = {
    # 背景色 - 暗色: #1e1e1e, 亮色: #f3f3f3
    ColorKey.BG_BASE: (QColor(30, 30, 30), QColor(243, 243, 243)),
    ColorKey.BG_TIMELINE: (QColor(30, 30, 30), QColor(243, 243, 243)),
    ColorKey.BG_TRACK_CONTROLS: (QColor(30, 30, 30), QColor(243, 243, 243)),
    ColorKey.BG_TRACK_ALT: (QColor(40, 40, 40), QColor(238, 238, 238)),
    ColorKey.BG_CONTROL_GROUP: (QColor(45, 45, 45), QColor(230, 230, 230)),
    ColorKey.BG_CLIP: (QColor(64, 68, 85), QColor(180, 185, 200)),
    # 文字色
    ColorKey.TEXT_PRIMARY: (QColor(224, 224, 224), QColor(32, 32, 32)),
    ColorKey.TEXT_SECONDARY: (QColor(136, 136, 136), QColor(96, 96, 96)),
    # Chart 图表色
    ColorKey.CHART_BG: (QColor(35, 35, 35), QColor(255, 255, 255)),
    ColorKey.CHART_GRID: (QColor(50, 50, 50), QColor(230, 230, 230)),
    ColorKey.CHART_LINE: QColor(0, 120, 212),  # 固有色：主题蓝
    ColorKey.CHART_LINE_SECONDARY: QColor(255, 140, 0),  # 固有色：橙色
    ColorKey.CHART_TEXT: (QColor(180, 180, 180), QColor(80, 80, 80)),
}


def get_color(key: ColorKey) -> QColor:
    """
    获取主题适配的颜色

    Args:
        key: 颜色键枚举

    Returns:
        QColor 对象
    """
    value = _COLOR_DEFINITIONS.get(key)
    if value is None:
        return QColor(128, 128, 128)

    if isinstance(value, QColor):
        return value

    dark_color, light_color = value
    return dark_color if isDarkTheme() else light_color


def get_color_hex(key: ColorKey) -> str:
    """
    获取主题适配的颜色 (十六进制格式)

    Args:
        key: 颜色键枚举

    Returns:
        十六进制颜色字符串 (#RRGGBB)
    """
    return get_color(key).name()


def get_accent_color() -> QColor:
    """
    获取系统主题色 (强调色)

    Returns:
        QColor 对象
    """
    return themeColor()


def get_accent_color_hex() -> str:
    """
    获取系统主题色 (十六进制格式)

    Returns:
        十六进制颜色字符串 (#RRGGBB)
    """
    return themeColor().name()
