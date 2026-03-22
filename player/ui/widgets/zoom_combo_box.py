"""
ZoomComboBox - 缩放控制 ComboBox

显示: [100%][下拉按钮]
预设: fit, 100%, 200%, 300%, ..., 1000%
"""
from PySide6.QtCore import Signal, QEvent, Qt
from PySide6.QtWidgets import QApplication

from qfluentwidgets_nuitka import EditableComboBox


class ZoomComboBox(EditableComboBox):
    """缩放控制 ComboBox

    显示: [100%][下拉按钮]
    预设: fit, 100%, 200%, 300%, ..., 1000%

    特性:
    - 支持用户输入任意缩放值（百分比格式如 "150%" 或小数格式如 "1.5"）
    - 预设选择 "Fit" 时自动计算实际缩放值
    - 最小值限制为当前 fit 计算值，确保画面不会太小
    """

    # 信号
    zoom_changed = Signal(float)  # 缩放比例变化，1.0 = 100%

    # 预设缩放值
    PRESET_ZOOMS = [
        ("Fit", "fit"),
        ("100%", "1.0"),
        ("200%", "2.0"),
        ("300%", "3.0"),
        ("400%", "4.0"),
        ("500%", "5.0"),
        ("600%", "6.0"),
        ("700%", "7.0"),
        ("800%", "8.0"),
        ("900%", "9.0"),
        ("1000%", "10.0"),
    ]

    def __init__(self, parent=None):
        super().__init__(parent)
        self._fit_value: float = 1.0  # 当前 fit 计算值
        self._suppress_signals = False  # 阻止信号发送标志
        self._last_valid_text: str = "100%"  # 上一次有效值

        # 设置固定宽度
        self.setFixedWidth(90)

        # 添加预设项
        for display_text, _ in self.PRESET_ZOOMS:
            self.addItem(display_text)

        # 初始值
        self.setText("100%")

        # 连接信号
        self.currentIndexChanged.connect(self._on_index_changed)  # 下拉选择
        self.editingFinished.connect(self._on_editing_finished)  # 用户编辑完成

        # 安装应用程序级别的事件过滤器，用于检测点击外部
        QApplication.instance().installEventFilter(self)

    def eventFilter(self, obj, event):
        """事件过滤器：检测点击外部时退出编辑状态"""
        if event.type() == QEvent.Type.MouseButtonPress:
            # 检查点击是否在控件外部
            if self.hasFocus() and not self.rect().contains(self.mapFromGlobal(event.globalPos())):
                self._finish_editing()
        return super().eventFilter(obj, event)

    def _finish_editing(self):
        """完成编辑（处理输入并清除焦点）"""
        if self._suppress_signals:
            return

        text = self.currentText().strip()
        try:
            ratio = self._parse_zoom_value(text)
            ratio = max(ratio, self._fit_value)
            processed_text = f"{int(ratio * 100)}%"
            self._last_valid_text = processed_text

            if processed_text != text:
                self._suppress_signals = True
                self.setText(processed_text)
                self._suppress_signals = False

            self._emit_zoom(processed_text)
        except ValueError:
            # 解析失败时恢复上一次有效值
            self._suppress_signals = True
            self.setText(self._last_valid_text)
            self._suppress_signals = False

        self.clearFocus()

    def set_fit_value(self, fit_value: float):
        """设置当前 fit 计算值（动态更新）

        当 viewport 大小变化时调用此方法更新 fit 值
        """
        self._fit_value = fit_value

    def get_fit_value(self) -> float:
        """获取当前 fit 值"""
        return self._fit_value

    def set_zoom_ratio(self, ratio: float, emit: bool = True):
        """设置缩放比例

        Args:
            ratio: 缩放比例 (1.0 = 100%)
            emit: 是否发射信号
        """
        # 钳制到最小值
        ratio = max(ratio, self._fit_value)
        display_text = f"{int(ratio * 100)}%"
        self._last_valid_text = display_text
        if not emit:
            self._suppress_signals = True
        self.setText(display_text)
        self._suppress_signals = False

    def get_zoom_ratio(self) -> float:
        """获取当前缩放比例"""
        return self._parse_zoom_value(self.currentText())

    def _on_index_changed(self, index: int):
        """下拉选择回调"""
        if self._suppress_signals or index < 0:
            return

        text = self.itemText(index)
        # 如果选择的是 Fit，切换到实际值
        if text == "Fit":
            actual_text = f"{int(self._fit_value * 100)}%"
            self._suppress_signals = True
            self.setText(actual_text)
            self._suppress_signals = False
            self._last_valid_text = actual_text
            self._emit_zoom(actual_text)
        else:
            self._last_valid_text = text
            self._emit_zoom(text)

    def _on_editing_finished(self):
        """编辑完成回调（回车或Tab触发）"""
        self._finish_editing()

    def _emit_zoom(self, text: str):
        """发射 zoom_changed 信号"""
        try:
            ratio = self._parse_zoom_value(text)
            ratio = max(ratio, self._fit_value)
            self.zoom_changed.emit(ratio)
        except ValueError:
            self.zoom_changed.emit(self._fit_value)

    def _parse_zoom_value(self, value: str) -> float:
        """解析缩放值字符串为比例

        支持格式:
        - "150%" -> 1.5
        - "1.5" -> 1.5
        - "150" -> 1.5 (无 % 符号时假设为百分比)

        Raises:
            ValueError: 无法解析时
        """
        value = value.strip()
        if not value:
            raise ValueError("Empty value")

        # 移除百分号
        value = value.rstrip('%')

        try:
            num = float(value)
            # 如果值 > 10，假设用户输入的是百分比（如 150 -> 150%）
            if num > 10:
                num = num / 100
            return num
        except ValueError:
            raise ValueError(f"Cannot parse zoom value: {value}")
