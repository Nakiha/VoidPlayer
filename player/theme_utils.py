"""
ThemeUtils - 主题工具模块
动态适配亮色/暗色主题的颜色系统
"""
from qfluentwidgets import isDarkTheme, themeColor
from PySide6.QtGui import QColor


def get_color(color_key: str) -> QColor:
    """
    获取主题适配的颜色

    Args:
        color_key: 颜色键名

    Returns:
        QColor 对象
    """
    dark = isDarkTheme()

    colors = {
        # 背景色
        "bg_base": QColor(30, 30, 30) if dark else QColor(243, 243, 243),
        "bg_elevated": QColor(45, 45, 45) if dark else QColor(255, 255, 255),
        "bg_controls": QColor(37, 37, 37) if dark else QColor(249, 249, 249),
        "bg_timeline": QColor(43, 43, 43) if dark else QColor(245, 245, 245),
        "bg_track_controls": QColor(51, 51, 51) if dark else QColor(240, 240, 240),
        "bg_control_group": QColor(51, 51, 51) if dark else QColor(230, 230, 230),
        "bg_video_placeholder": QColor(34, 34, 34) if dark else QColor(200, 200, 200),
        "bg_viewport": QColor(0, 0, 0) if dark else QColor(24, 24, 24),
        "bg_track_content": QColor(26, 26, 26) if dark else QColor(220, 220, 220),
        "bg_clip": QColor(64, 68, 85) if dark else QColor(180, 185, 200),

        # 文字色
        "text_primary": QColor(224, 224, 224) if dark else QColor(32, 32, 32),
        "text_secondary": QColor(136, 136, 136) if dark else QColor(96, 96, 96),

        # 边框色
        "border": QColor(62, 62, 62) if dark else QColor(229, 229, 229),
        "border_strong": QColor(80, 80, 80) if dark else QColor(200, 200, 200),
    }

    return colors.get(color_key, QColor(128, 128, 128))


def get_color_hex(color_key: str) -> str:
    """
    获取主题适配的颜色 (十六进制格式)

    Args:
        color_key: 颜色键名

    Returns:
        十六进制颜色字符串 (#RRGGBB)
    """
    return get_color(color_key).name()


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
