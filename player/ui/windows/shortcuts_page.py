"""
ShortcutsPage - 快捷键展示页面
使用轻量 QWidget 行 + qfluent 滚动视图
"""
from typing import TYPE_CHECKING

from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QFrame
from PySide6.QtCore import Qt, QVariantAnimation, QEasingCurve
from PySide6.QtGui import QPainter, QColor, QBrush, QPen, QFont, QFontMetrics
from qfluentwidgets_nuitka import ScrollArea, BodyLabel, StrongBodyLabel, isDarkTheme

from player.ui.theme_utils import get_color_hex, ColorKey

if TYPE_CHECKING:
    from player.core.shortcuts import ShortcutManager


class ShortcutBadge(QWidget):
    """快捷键徽章 - 抗锯齿圆角边框"""

    def __init__(self, text: str, parent=None):
        super().__init__(parent)
        self._text = text
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)

        # 计算尺寸
        self._font = QFont("Consolas")
        self._font.setPixelSize(13)
        fm = QFontMetrics(self._font)
        text_width = fm.horizontalAdvance(text)
        text_height = fm.height()
        self.setFixedSize(text_width + 22, text_height + 10)

    def paintEvent(self, _event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        # 绘制圆角边框
        rect = self.rect().adjusted(1, 1, -1, -1)
        border_color = QColor(get_color_hex(ColorKey.BORDER))
        painter.setPen(QPen(border_color, 1))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRoundedRect(rect, 4, 4)

        # 绘制文字
        text_color = QColor(get_color_hex(ColorKey.TEXT_SECONDARY))
        painter.setPen(text_color)
        painter.setFont(self._font)
        painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, self._text)


class ShortcutRow(QWidget):
    """单行快捷键 - 带 hover 高亮动画"""

    def __init__(self, description: str, shortcut: str, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setMouseTracking(True)
        self._hover_opacity = 0.0
        self._animation = QVariantAnimation(self)
        self._animation.setDuration(150)
        self._animation.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._animation.valueChanged.connect(self._on_opacity_changed)
        self._setup_ui(description, shortcut)

    def _on_opacity_changed(self, value):
        self._hover_opacity = float(value)
        self.update()

    def _get_hover_color(self) -> QColor:
        """获取 hover 高亮色"""
        if isDarkTheme():
            return QColor(255, 255, 255, int(255 * 0.08))
        else:
            return QColor(0, 0, 0, int(255 * 0.06))

    def enterEvent(self, event):
        self._animation.stop()
        self._animation.setStartValue(self._hover_opacity)
        self._animation.setEndValue(1.0)
        self._animation.start()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self._animation.stop()
        self._animation.setStartValue(self._hover_opacity)
        self._animation.setEndValue(0.0)
        self._animation.start()
        super().leaveEvent(event)

    def paintEvent(self, event):
        """绘制圆角高亮背景"""
        if self._hover_opacity > 0:
            painter = QPainter(self)
            painter.setRenderHint(QPainter.Antialiasing)
            color = self._get_hover_color()
            color.setAlpha(int(color.alpha() * self._hover_opacity))
            painter.setBrush(QBrush(color))
            painter.setPen(Qt.PenStyle.NoPen)
            painter.drawRoundedRect(self.rect(), 4, 4)
        super().paintEvent(event)

    def _setup_ui(self, description: str, shortcut: str):
        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(8)

        # 左侧：功能描述
        desc_label = BodyLabel(description)
        desc_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        layout.addWidget(desc_label)
        layout.addStretch()

        # 右侧：快捷键徽章（抗锯齿）
        key_badge = ShortcutBadge(shortcut)
        layout.addWidget(key_badge)


class ShortcutsPage(ScrollArea):
    """快捷键页面 - 展示所有可用快捷键"""

    def __init__(self, shortcut_manager: "ShortcutManager", parent=None):
        super().__init__(parent)
        self.setObjectName("ShortcutsPage")
        self._shortcut_manager = shortcut_manager
        self._setup_ui()

    def _setup_ui(self):
        """设置 UI"""
        self.setWidgetResizable(True)
        self.setStyleSheet("QScrollArea { border: none; background: transparent; }")

        # 修复透明背景滚动残留
        self.viewport().setStyleSheet("background: transparent;")
        self.viewport().setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.viewport().setAutoFillBackground(False)

        # 容器 - 透明背景
        container = QWidget()
        container.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        layout = QVBoxLayout(container)
        layout.setContentsMargins(16, 16, 16, 16)
        layout.setSpacing(4)

        # 获取所有快捷键 (key, desc, category)
        shortcuts = self._shortcut_manager.get_all_shortcuts_info()

        # 分类顺序
        category_order = ["播放控制", "速度控制", "缩放控制", "项目操作", "其他"]

        # 按类别分组
        categories: dict[str, list] = {cat: [] for cat in category_order}
        for key, desc, cat in shortcuts:
            if cat in categories:
                categories[cat].append((key, desc))

        # 按类别填充
        for cat in category_order:
            items = categories[cat]
            if not items:
                continue

            # 类别标题
            cat_label = StrongBodyLabel(cat)
            cat_label.setStyleSheet(f"color: {get_color_hex(ColorKey.TEXT_SECONDARY)}; margin-top: 8px;")
            layout.addWidget(cat_label)

            # 分隔线
            separator = QFrame()
            separator.setFixedHeight(2)
            separator.setStyleSheet(f"""
                QFrame {{
                    background-color: {get_color_hex(ColorKey.BORDER)};
                    max-width: 16777215;
                }}
            """)
            layout.addWidget(separator)

            # 快捷键行
            for key, desc in items:
                row = ShortcutRow(desc, key)
                layout.addWidget(row)

        layout.addStretch()
        self.setWidget(container)
