"""
VoidPlayer 播放器启动脚本
"""
import sys
import ctypes
from ctypes import wintypes
from pathlib import Path
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from qfluentwidgets import setThemeColor, setTheme, Theme

from player.logging_config import setup_logging
from player.main_window import MainWindow


def get_windows_accent_color() -> str:
    """
    获取 Windows 系统主题色 (强调色)
    返回格式: #RRGGBB
    """
    try:
        # 使用 Windows API 获取系统主题色
        # ImmGetColor() 可以获取强调色，但需要 dwmapi.dll
        # 更简单的方法是读取注册表
        import winreg

        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\DWM"
        )

        # AccentColor 的值是 0xAABBGGRR 格式 (ABGR)
        value, _ = winreg.QueryValueEx(key, "AccentColor")

        # 转换为 RGB
        # Windows 返回的是 ABGR 格式，需要转换为 RGB
        if value & 0xFF000000:  # 有 Alpha 通道
            # 取消 Alpha，并从 BGR 转为 RGB
            r = value & 0xFF
            g = (value >> 8) & 0xFF
            b = (value >> 16) & 0xFF
        else:
            # 直接从 BGR 转为 RGB
            r = value & 0xFF
            g = (value >> 8) & 0xFF
            b = (value >> 16) & 0xFF

        return f"#{r:02x}{g:02x}{b:02x}"

    except Exception:
        # 获取失败时返回默认蓝色 (Windows 11 默认主题色)
        return "#0078d4"


def get_windows_dark_mode() -> bool:
    """
    检测 Windows 是否使用暗色主题
    返回: True 表示暗色主题
    """
    try:
        import winreg

        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"
        )

        # AppsUseLightTheme: 0 = 暗色, 1 = 亮色
        value, _ = winreg.QueryValueEx(key, "AppsUseLightTheme")

        return value == 0

    except Exception:
        # 默认使用暗色主题
        return True


def main():
    # 初始化日志系统 (开发模式使用项目目录下的 logs 文件夹)
    # 设置 dev_mode=False 使用用户数据目录
    setup_logging(app_name="voidplayer", dev_mode=True)

    # 启用高 DPI 缩放
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough
    )

    app = QApplication(sys.argv)

    # 获取并设置 Windows 系统主题色
    accent_color = get_windows_accent_color()
    setThemeColor(accent_color)

    # 设置主题跟随系统 (自动检测亮色/暗色)
    setTheme(Theme.AUTO)

    # 创建主窗口
    window = MainWindow()
    window.show()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
