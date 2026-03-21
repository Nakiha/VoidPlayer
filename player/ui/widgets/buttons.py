"""
按钮工具函数
"""
from qfluentwidgets_nuitka import TransparentToolButton, FluentIcon, ToolTipFilter


def create_tool_button(icon: FluentIcon, parent=None, size: int = 28, tooltip: str = "") -> TransparentToolButton:
    """
    创建一个修复了字体问题的 TransparentToolButton

    修复: qfluentwidgets 的 TransparentToolButton 在悬停时会触发
    QFont::setPointSize: Point size <= 0 (-1) 警告

    Args:
        icon: FluentIcon 图标
        parent: 父控件
        size: 按钮大小
        tooltip: 提示文本 (可选)

    Returns:
        TransparentToolButton 实例
    """
    btn = TransparentToolButton(parent)
    btn.setIcon(icon)
    btn.setFixedSize(size, size)

    # 修复字体问题: 设置一个有效的字体确保 pointSize > 0
    font = btn.font()
    if font.pointSize() <= 0:
        font.setPointSize(9)  # 设置一个默认的有效字体大小
        btn.setFont(font)

    # 设置 tooltip (使用 Fluent 风格)
    if tooltip:
        btn.setToolTip(tooltip)
        btn.installEventFilter(ToolTipFilter(btn, 0))

    return btn
