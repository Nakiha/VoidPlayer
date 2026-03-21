"""
ElideComboBox - 支持文本省略的下拉框控件
"""
import os
from PySide6.QtCore import Qt
from PySide6.QtGui import QFontMetrics
from qfluentwidgets_nuitka import ComboBox


class ElideComboBox(ComboBox):
    """支持文本省略的下拉框 - 显示名称过长时自动截断，悬停显示完整tooltip"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items: list[str] = []  # 存储完整路径
        self._display_texts: list[str] = []  # 存储显示名称（basename）
        self._current_index = -1

    def addItem(self, text: str, userData=None):
        """添加项目（text 为完整路径）"""
        self._items.append(text)
        display = os.path.basename(text)
        self._display_texts.append(display)
        super().addItem(display, userData)
        self._update_tooltip()

    def addItems(self, texts: list[str]):
        """添加多个项目"""
        for text in texts:
            self.addItem(text)

    def clear(self):
        """清空所有项目"""
        self._items.clear()
        self._display_texts.clear()
        self._current_index = -1
        super().clear()

    def setCurrentIndex(self, index: int):
        """设置当前选中索引"""
        self._current_index = index
        super().setCurrentIndex(index)
        self._update_tooltip()

    def currentItem(self) -> str:
        """获取当前选中项的完整路径"""
        if 0 <= self._current_index < len(self._items):
            return self._items[self._current_index]
        return ""

    def itemPath(self, index: int) -> str:
        """获取指定索引的完整路径"""
        if 0 <= index < len(self._items):
            return self._items[index]
        return ""

    def resizeEvent(self, event):
        """调整大小时重新计算省略文本"""
        super().resizeEvent(event)
        self._update_elided_text()

    def _update_tooltip(self):
        """更新 tooltip 显示完整路径"""
        if 0 <= self._current_index < len(self._items):
            self.setToolTip(self._items[self._current_index])
        else:
            self.setToolTip("")

    def _update_elided_text(self):
        """根据可用宽度更新显示文本"""
        if not self._display_texts:
            return

        # 获取可用宽度（减去下拉箭头和边距）
        available_width = self.width() - 40  # 预留箭头和边距空间
        if available_width <= 0:
            return

        # 重新设置所有项目的显示文本
        fm = QFontMetrics(self.font())
        self.blockSignals(True)
        current = self.currentIndex()

        # 清空并重新添加省略后的文本
        super().clear()
        for display in self._display_texts:
            elided = fm.elidedText(display, Qt.TextElideMode.ElideMiddle, available_width)
            super().addItem(elided)

        super().setCurrentIndex(current)
        self.blockSignals(False)
