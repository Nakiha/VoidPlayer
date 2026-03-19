"""
StatsWindow - 性能统计窗口

显示播放性能统计信息，类似 mpv 的 stats.lua:
- 实际帧率
- 解码时间
- 帧丢弃/延迟统计
- 性能警告指示
- 导出诊断报告

使用方法:
- 按 'I' 键切换显示
"""
from pathlib import Path

from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QLabel, QHBoxLayout, QPushButton,
    QTableWidget, QTableWidgetItem, QHeaderView, QGroupBox
)
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QFont, QCloseEvent, QColor

from player.theme_utils import get_color_hex, ColorKey
from player.core.logging_config import get_logger


class StatsWindow(QWidget):
    """
    性能统计窗口 (独立窗口)

    显示:
    - 每个轨道的实际帧率
    - 解码时间 (平均/最大)
    - 延迟帧/总帧数
    - 性能状态指示
    - 导出诊断报告按钮
    """

    def __init__(self, parent=None):
        super().__init__(parent)

        self._stats_data = {}
        self._diagnostics_manager = None
        self._logger = get_logger()

        self._setup_ui()

        # 更新定时器
        self._update_timer = QTimer(self)
        self._update_timer.timeout.connect(self._update_display)

    def _setup_ui(self):
        """设置 UI"""
        # 独立窗口
        self.setWindowFlags(
            Qt.WindowType.Window |
            Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, False)

        # 窗口属性
        self.setWindowTitle("Performance Stats")
        self.setMinimumSize(400, 200)
        self.resize(500, 300)

        # 样式
        self.setStyleSheet(f"""
            QWidget {{
                background-color: {get_color_hex(ColorKey.BG_BASE)};
                color: {get_color_hex(ColorKey.TEXT_PRIMARY)};
            }}
            QGroupBox {{
                font-weight: bold;
                border: 1px solid {get_color_hex(ColorKey.BORDER)};
                border-radius: 4px;
                margin-top: 8px;
                padding-top: 8px;
            }}
            QGroupBox::title {{
                subcontrol-origin: margin;
                left: 8px;
                padding: 0 4px;
            }}
            QPushButton {{
                background-color: {get_color_hex(ColorKey.BG_SECONDARY)};
                border: 1px solid {get_color_hex(ColorKey.BORDER)};
                border-radius: 4px;
                padding: 6px 12px;
                color: {get_color_hex(ColorKey.TEXT_PRIMARY)};
            }}
            QPushButton:hover {{
                background-color: {get_color_hex(ColorKey.BG_TERTIARY)};
            }}
            QPushButton:checked {{
                background-color: {get_color_hex(ColorKey.ACCENT)};
            }}
            QTableWidget {{
                background-color: {get_color_hex(ColorKey.BG_SECONDARY)};
                border: 1px solid {get_color_hex(ColorKey.BORDER)};
                gridline-color: {get_color_hex(ColorKey.BORDER)};
            }}
            QTableWidget::item {{
                padding: 4px;
            }}
            QHeaderView::section {{
                background-color: {get_color_hex(ColorKey.BG_TERTIARY)};
                border: none;
                padding: 4px;
                font-weight: bold;
            }}
        """)

        # 主布局
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(8)

        # 工具栏
        toolbar = QWidget()
        toolbar_layout = QHBoxLayout(toolbar)
        toolbar_layout.setContentsMargins(0, 0, 0, 0)
        toolbar_layout.setSpacing(8)

        # 导出按钮
        self._export_btn = QPushButton("Export Now")
        self._export_btn.clicked.connect(self._on_export_clicked)
        toolbar_layout.addWidget(self._export_btn)

        # 导出状态开关
        self._auto_export_btn = QPushButton("Auto Export: OFF")
        self._auto_export_btn.setCheckable(True)
        self._auto_export_btn.clicked.connect(self._on_auto_export_toggled)
        toolbar_layout.addWidget(self._auto_export_btn)

        toolbar_layout.addStretch()

        # 导出状态标签
        self._export_status_label = QLabel("")
        self._export_status_label.setStyleSheet(f"color: {get_color_hex(ColorKey.TEXT_SECONDARY)};")
        toolbar_layout.addWidget(self._export_status_label)

        layout.addWidget(toolbar)

        # 统计表格
        stats_group = QGroupBox("Track Statistics")
        stats_layout = QVBoxLayout(stats_group)
        stats_layout.setContentsMargins(8, 12, 8, 8)

        self._table = QTableWidget()
        self._table.setColumnCount(7)
        self._table.setHorizontalHeaderLabels([
            "Track", "FPS", "Target", "Decode Avg", "Decode Max", "Late/Total", "Status"
        ])
        self._table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._table.setAlternatingRowColors(True)

        stats_layout.addWidget(self._table)
        layout.addWidget(stats_group)

        # 播放时间
        self._playback_time_label = QLabel("Playback: 0.0s")
        self._playback_time_label.setStyleSheet(f"color: {get_color_hex(ColorKey.TEXT_SECONDARY)};")
        layout.addWidget(self._playback_time_label)

    def set_diagnostics_manager(self, manager):
        """设置诊断管理器"""
        self._diagnostics_manager = manager
        if manager:
            manager.stats_updated.connect(self.update_stats)
            manager.export_completed.connect(self._on_export_completed)

    def show_window(self):
        """显示窗口"""
        self.show()
        self.raise_()
        self.activateWindow()
        self._update_timer.start(500)

    def hide_window(self):
        """隐藏窗口"""
        self._update_timer.stop()
        self.hide()

    def toggle_window(self):
        """切换显示"""
        if self.isVisible():
            self.hide_window()
        else:
            self.show_window()

    def closeEvent(self, event: QCloseEvent):
        """关闭事件 - 隐藏而非关闭"""
        event.ignore()
        self.hide_window()

    def update_stats(self, stats_data: dict):
        """更新统计数据"""
        self._stats_data = stats_data
        if self.isVisible():
            self._update_display()

    def _update_display(self):
        """更新显示"""
        tracks = self._stats_data.get("tracks", {})
        playback_time = self._stats_data.get("playback_time", 0)

        # 更新播放时间
        self._playback_time_label.setText(f"Playback: {playback_time:.1f}s")

        # 更新表格
        self._table.setRowCount(len(tracks))

        for row, (track_idx, track_stats) in enumerate(tracks.items()):
            # 解码时间占比
            decode_ratio = track_stats["avg_decode_time_ms"] / track_stats["frame_interval_ms"] if track_stats["frame_interval_ms"] > 0 else 0

            # 性能状态
            if track_stats["is_bottleneck"]:
                status = "Bottleneck!"
                status_color = "#ff6b6b"
            elif decode_ratio > 0.6:
                status = "Warning"
                status_color = "#ffd43b"
            else:
                status = "OK"
                status_color = "#69db7c"

            # 表格项
            items = [
                str(track_idx),
                f"{track_stats['current_fps']:.1f}",
                f"{track_stats['fps']:.1f}",
                f"{track_stats['avg_decode_time_ms']:.1f}ms",
                f"{track_stats['max_decode_time_ms']:.1f}ms",
                f"{track_stats['late_frames']}/{track_stats['total_frames']}",
                status,
            ]

            for col, text in enumerate(items):
                item = QTableWidgetItem(text)
                item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                if col == 6:  # 状态列
                    color = QColor(0x69, 0xdb, 0x7c) if status == "OK" else \
                            QColor(0xff, 0x6b, 0x6b) if status == "Bottleneck!" else \
                            QColor(0xff, 0xd4, 0x3b)
                    item.setForeground(color)
                self._table.setItem(row, col, item)

    def _on_export_clicked(self):
        """导出按钮点击"""
        if not self._diagnostics_manager:
            return

        file_path = self._diagnostics_manager.export_now()
        if file_path:
            self._export_status_label.setText(f"Exported: {file_path}")
        else:
            self._export_status_label.setText("No data to export (play video first)")

    def _on_auto_export_toggled(self, checked: bool):
        """自动导出开关"""
        if self._diagnostics_manager:
            self._diagnostics_manager.set_export_enabled(checked)
            self._auto_export_btn.setText(f"Auto Export: {'ON' if checked else 'OFF'}")
            if checked:
                self._export_status_label.setText(f"Auto export enabled -> {self._diagnostics_manager._export_dir}")

    def _on_export_completed(self, file_path: str):
        """导出完成回调"""
        self._export_status_label.setText(f"Exported: {file_path}")
        self._logger.info(f"[StatsWindow] Export completed: {file_path}")


# 兼容旧名称
StatsOverlay = StatsWindow
