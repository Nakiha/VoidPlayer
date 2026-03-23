"""
SettingsWindow - 设置窗口
"""
from typing import TYPE_CHECKING

from qfluentwidgets_nuitka import (
    FluentWindow,
    NavigationItemPosition,
    FluentIcon,
)

from .shortcuts_page import ShortcutsPage

if TYPE_CHECKING:
    from player.core.shortcuts import ShortcutManager


class SettingsWindow(FluentWindow):
    """设置窗口"""

    def __init__(self, shortcut_manager: "ShortcutManager", parent=None):
        super().__init__(parent)
        self._shortcut_manager = shortcut_manager
        self._setup_ui()

    def _setup_ui(self):
        """设置 UI"""
        self.setWindowTitle("设置")
        self.setMinimumSize(500, 400)
        self.resize(600, 500)

        # 快捷键页面
        self.shortcuts_page = ShortcutsPage(self._shortcut_manager, self)
        self.addSubInterface(
            self.shortcuts_page,
            FluentIcon.SETTING,
            "快捷键",
            NavigationItemPosition.TOP,
        )
