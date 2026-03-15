"""
ElideLabel - 支持文本省略的标签控件
"""
from PySide6.QtWidgets import QLabel
from PySide6.QtCore import Qt
from qfluentwidgets import ToolTipFilter


class ElideLabel(QLabel):
    """支持文本省略的标签 - 文本过长时显示"..."，悬停显示完整tooltip"""

    def __init__(self, text: str = "", parent=None):
        super().__init__(text, parent)
        self._full_text = text
        # 安装 Fluent 风格的 ToolTipFilter
        self.installEventFilter(ToolTipFilter(self, 0))
        self.setText(text)

    def setText(self, text: str):
        """设置文本"""
        self._full_text = text
        super().setText(text)
        # 设置 tooltip 显示完整文本
        if text:
            self.setToolTip(text)
        else:
            self.setToolTip("")

    def resizeEvent(self, event):
        """调整大小时重新计算省略文本"""
        super().resizeEvent(event)
        self._update_elided_text()

    def _update_elided_text(self):
        """根据可用宽度更新省略文本"""
        if not self._full_text:
            return

        # 获取可用宽度（减去边距）
        available_width = self.width()
        if available_width <= 0:
            return

        # 使用字体度量计算省略文本
        fm = self.fontMetrics()
        elided = fm.elidedText(self._full_text, Qt.TextElideMode.ElideRight, available_width)

        # 只有当文本被省略时才使用省略版本
        super().setText(elided)
