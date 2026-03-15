"""
可切换状态的工具按钮
"""
from PySide6.QtCore import Qt, QPropertyAnimation, Property, QEasingCurve
from PySide6.QtGui import QColor, QBrush, QPainter
from qfluentwidgets import TransparentToolButton, FluentIcon, isDarkTheme, themeColor


class CheckableToolButton(TransparentToolButton):
    """
    可切换状态的工具按钮

    支持 checked 状态下自动显示主题色背景高亮
    内置 hover/pressed 动画效果
    """

    def __init__(self, icon: FluentIcon, parent=None):
        super().__init__(parent)
        self.setIcon(icon)
        self.setCheckable(True)

        # 背景透明度动画
        self._bg_alpha = 0
        self._target_alpha = 0
        self._anim = QPropertyAnimation(self, b"bgAlpha", self)
        self._anim.setDuration(150)
        self._anim.setEasingCurve(QEasingCurve.OutCubic)

    def getBgAlpha(self):
        return self._bg_alpha

    def setBgAlpha(self, value):
        self._bg_alpha = value
        self.update()

    bgAlpha = Property(int, getBgAlpha, setBgAlpha)

    def _get_base_color(self) -> QColor:
        """获取基础颜色 (checked 用主题色，否则用白/黑)"""
        if self.isChecked():
            color = QColor(themeColor())
        elif isDarkTheme():
            color = QColor(255, 255, 255)
        else:
            color = QColor(0, 0, 0)
        return color

    def _animate_to(self, alpha: int):
        """动画过渡到目标透明度"""
        self._target_alpha = alpha
        self._anim.stop()
        self._anim.setStartValue(self._bg_alpha)
        self._anim.setEndValue(alpha)
        self._anim.start()

    def enterEvent(self, e):
        super().enterEvent(e)
        if not self.isChecked():
            self._animate_to(20)
        else:
            self._animate_to(100)

    def leaveEvent(self, e):
        super().leaveEvent(e)
        if not self.isChecked():
            self._animate_to(0)
        else:
            self._animate_to(60)

    def mousePressEvent(self, e):
        super().mousePressEvent(e)
        if e.button() == Qt.LeftButton:
            self._animate_to(140 if self.isChecked() else 40)

    def mouseReleaseEvent(self, e):
        super().mouseReleaseEvent(e)
        if e.button() == Qt.LeftButton:
            # 状态已切换，根据新状态设置透明度
            if self.isChecked():
                self._animate_to(100)  # hover + checked
            else:
                self._animate_to(20)   # hover only

    def setChecked(self, checked: bool):
        """重写 setChecked，触发样式更新"""
        old_checked = self.isChecked()
        super().setChecked(checked)
        if old_checked != checked:
            self._update_checked_style()

    def _update_checked_style(self):
        """checked 状态变化时更新背景"""
        if self.isChecked():
            self._animate_to(60 if not self.underMouse() else 100)
        else:
            self._animate_to(0 if not self.underMouse() else 20)

    def paintEvent(self, e):
        """绘制背景 + 原有内容"""
        if self._bg_alpha > 0:
            painter = QPainter(self)
            painter.setRenderHint(QPainter.Antialiasing)

            color = self._get_base_color()
            color.setAlpha(self._bg_alpha)
            painter.setBrush(QBrush(color))
            painter.setPen(Qt.NoPen)
            painter.drawEllipse(self.rect())

        super().paintEvent(e)
