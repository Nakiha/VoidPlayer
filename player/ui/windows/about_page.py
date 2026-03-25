"""
AboutPage - 关于本项目页面
"""
import re
from pathlib import Path

from PySide6.QtWidgets import QWidget, QVBoxLayout, QLabel
from PySide6.QtCore import Qt, QUrl
from qfluentwidgets_nuitka import ScrollArea, StrongBodyLabel, HyperlinkLabel

from player.ui.theme_utils import get_color_hex, ColorKey


def get_version() -> str:
    """从 pyproject.toml 获取版本号"""
    try:
        pyproject = Path(__file__).parent.parent.parent.parent / "pyproject.toml"
        content = pyproject.read_text(encoding="utf-8")
        match = re.search(r'version\s*=\s*"([^"]+)"', content)
        if match:
            return match.group(1)
    except Exception:
        pass
    return "unknown"


class AboutPage(ScrollArea):
    """关于本项目页面"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("AboutPage")
        self._setup_ui()

    def _setup_ui(self):
        """设置 UI"""
        self.setWidgetResizable(True)
        self.setStyleSheet("QScrollArea { border: none; background: transparent; }")

        # 修复透明背景滚动残留
        self.viewport().setStyleSheet("background: transparent;")
        self.viewport().setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.viewport().setAutoFillBackground(False)

        # 容器
        container = QWidget()
        container.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        layout = QVBoxLayout(container)
        layout.setContentsMargins(16, 16, 16, 16)
        layout.setSpacing(4)

        # 项目名称和版本
        title = StrongBodyLabel(f"VoidPlayer v{get_version()}")
        title.setStyleSheet(f"font-size: 18px; color: {get_color_hex(ColorKey.TEXT_PRIMARY)};")
        layout.addWidget(title)

        layout.addSpacing(8)

        # 描述
        desc = QLabel("多视频同步播放器")
        desc.setStyleSheet(f"color: {get_color_hex(ColorKey.TEXT_SECONDARY)};")
        desc.setWordWrap(True)
        layout.addWidget(desc)

        # 源码链接
        source_link = HyperlinkLabel("源码仓库", self)
        source_link.setUrl(QUrl("https://github.com/Nakiha/VoidPlayer"))
        layout.addWidget(source_link)

        layout.addSpacing(16)

        # 版权声明
        copyright_label = QLabel("Copyright (C) 2024-2026 yorune")
        copyright_label.setStyleSheet(f"color: {get_color_hex(ColorKey.TEXT_SECONDARY)};")
        layout.addWidget(copyright_label)

        layout.addSpacing(16)

        # 许可证
        license_title = StrongBodyLabel("许可证")
        license_title.setStyleSheet(f"color: {get_color_hex(ColorKey.TEXT_SECONDARY)};")
        layout.addWidget(license_title)

        license_info = QLabel(
            "本程序是自由软件：你可以再分发之和/或依照由自由软件基金会发布的 "
            "GNU 通用公共许可证修改之，无论是版本 3 许可证，还是（按你的决定）"
            "任何以后版本都可以。"
        )
        license_info.setStyleSheet(f"color: {get_color_hex(ColorKey.TEXT_SECONDARY)};")
        license_info.setWordWrap(True)
        layout.addWidget(license_info)

        gpl_link = HyperlinkLabel("查看 GPLv3 许可证全文", self)
        gpl_link.setUrl(QUrl("https://www.gnu.org/licenses/gpl-3.0.html"))
        layout.addWidget(gpl_link)

        layout.addSpacing(16)

        # 主要依赖
        deps_title = StrongBodyLabel("主要依赖")
        deps_title.setStyleSheet(f"color: {get_color_hex(ColorKey.TEXT_SECONDARY)};")
        layout.addWidget(deps_title)

        deps = [
            ("FFmpeg", "https://ffmpeg.org", "GPLv3"),
            ("PySide6", "https://www.qt.io", "LGPLv3"),
            ("PyOpenGL", "https://pyopengl.sourceforge.net", "BSD"),
            ("PySide6-Fluent-Widgets", "https://github.com/zhiyiYo/PyQt-Fluent-Widgets", "Apache 2.0"),
        ]

        for name, url, license_type in deps:
            dep_layout = QVBoxLayout()
            dep_layout.setSpacing(0)
            dep_layout.setContentsMargins(0, 4, 0, 4)

            link = HyperlinkLabel(name, self)
            link.setUrl(QUrl(url))
            dep_layout.addWidget(link)

            dep_info = QLabel(f"许可证: {license_type}")
            dep_info.setStyleSheet(f"color: {get_color_hex(ColorKey.TEXT_SECONDARY)}; font-size: 12px;")
            dep_layout.addWidget(dep_info)

            # 将 QVBoxLayout 添加到主布局需要一个包装 widget
            dep_widget = QWidget()
            dep_widget.setLayout(dep_layout)
            layout.addWidget(dep_widget)

        layout.addStretch()
        self.setWidget(container)
