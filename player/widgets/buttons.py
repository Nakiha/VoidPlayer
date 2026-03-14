"""
按钮工具函数
"""
from qfluentwidgets import TransparentToolButton, FluentIcon


def create_tool_button(icon: FluentIcon, parent=None, size: int = 28) -> TransparentToolButton:
    """
    创建一个修复了字体问题的 TransparentToolButton

    修复: qfluentwidgets 的 TransparentToolButton 在悬停时会触发
    QFont::setPointSize: Point size <= 0 (-1) 警告

    Args:
        icon: FluentIcon 图标
        parent: 父控件
        size: 按钮大小

    Returns:
        TransparentToolButton 实例
    """
    btn = TransparentToolButton(icon, parent)
    btn.setFixedSize(size, size)

    # 修复字体问题: 设置一个有效的字体确保 pointSize > 0
    font = btn.font()
    if font.pointSize() <= 0:
        font.setPointSize(9)  # 设置一个默认的有效字体大小
        btn.setFont(font)

    return btn
