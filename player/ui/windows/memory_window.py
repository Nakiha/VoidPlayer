"""
MemoryWindow - 内存监控窗口

零开销设计:
- 窗口关闭时 = 停止所有监控 = 零开销
- psutil 读取内存几乎无开销 (~0.1ms)
- QObject 统计按需点击刷新
- tracemalloc 快照按需触发
"""
import os
import time
import tracemalloc
from collections import Counter
from typing import Optional

from PySide6.QtWidgets import (
    QWidget,
    QVBoxLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QTextEdit,
)
from PySide6.QtCore import QTimer, Qt, QRect
from PySide6.QtGui import QPainter, QPen, QFont

from player.ui.theme_utils import get_color, ColorKey
from player.ui.widgets import HighlightSplitter

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False


class MemoryChart(QWidget):
    """实时内存曲线图 - 使用 QPainter 轻量绘制"""

    # 图表边距 (左右多加 8px 与下方 widget 对齐)
    MARGIN_LEFT = 58
    MARGIN_RIGHT = 18
    MARGIN_TOP = 30
    MARGIN_BOTTOM = 30

    def __init__(self, parent=None):
        super().__init__(parent)
        self._max_points = 60
        # 存储 (time, uss, rss) 三元组
        self._data: list[tuple[float, float, float]] = []
        self._min_mem = 0.0
        self._max_mem = 100.0
        self._min_time = 0.0
        self._max_time = 60.0

        self.setMinimumHeight(150)

    def add_point(self, time: float, uss_mb: float, rss_mb: float):
        """添加数据点 (USS 私有内存, RSS 总内存)"""
        self._data.append((time, uss_mb, rss_mb))

        if len(self._data) > self._max_points:
            self._data.pop(0)

        # 更新范围
        if self._data:
            times = [t for t, _, _ in self._data]
            # 使用 RSS 计算范围（更大的值）
            mems = [rss for _, _, rss in self._data]
            self._min_time = min(times)
            self._max_time = max(max(times), self._min_time + 1)
            # Y 轴从 0 开始
            self._min_mem = 0
            self._max_mem = max(max(mems), 100)  # 至少显示 100MB

        self.update()

    def clear(self):
        """清除数据"""
        self._data.clear()
        self._min_mem = 0.0
        self._max_mem = 100.0
        self._min_time = 0.0
        self._max_time = 60.0
        self.update()

    def paintEvent(self, event):
        """绘制图表"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        rect = self.rect()

        # 绘图区域
        plot_rect = rect.adjusted(
            self.MARGIN_LEFT, self.MARGIN_TOP,
            -self.MARGIN_RIGHT, -self.MARGIN_BOTTOM
        )

        # 背景
        painter.fillRect(rect, get_color(ColorKey.CHART_BG))

        # 标题 - 居中于绘图区域上方
        painter.setPen(get_color(ColorKey.TEXT_PRIMARY))
        painter.setFont(QFont("Microsoft YaHei", 10, QFont.Bold))
        title_rect = QRect(plot_rect.left(), 0, plot_rect.width(), self.MARGIN_TOP)
        painter.drawText(title_rect, Qt.AlignHCenter | Qt.AlignVCenter, "内存使用 (MB)")

        # 绘制网格和坐标轴
        self._draw_grid(painter, plot_rect)
        self._draw_curve(painter, plot_rect)

    def _draw_grid(self, painter: QPainter, rect):
        """绘制网格和坐标轴标签"""
        # 网格线
        painter.setPen(QPen(get_color(ColorKey.CHART_GRID), 1))

        # 水平网格线 (4条)
        for i in range(5):
            y = rect.top() + i * rect.height() // 4
            painter.drawLine(rect.left(), y, rect.right(), y)

            # Y轴标签
            if self._max_mem > self._min_mem:
                val = self._max_mem - (self._max_mem - self._min_mem) * i / 4
                painter.setPen(get_color(ColorKey.CHART_TEXT))
                painter.setFont(QFont("Microsoft YaHei", 8))
                painter.drawText(5, y + 4, f"{val:.0f}")
                painter.setPen(QPen(get_color(ColorKey.CHART_GRID), 1))

        # 垂直网格线 (4条)
        for i in range(5):
            x = rect.left() + i * rect.width() // 4
            painter.drawLine(x, rect.top(), x, rect.bottom())

            # X轴标签
            if self._max_time > self._min_time:
                val = self._min_time + (self._max_time - self._min_time) * i / 4
                painter.setPen(get_color(ColorKey.CHART_TEXT))
                painter.setFont(QFont("Microsoft YaHei", 8))
                painter.drawText(x - 10, rect.bottom() + 15, f"{val:.0f}s")
                painter.setPen(QPen(get_color(ColorKey.CHART_GRID), 1))

    def _draw_curve(self, painter: QPainter, rect):
        """绘制曲线"""
        if len(self._data) < 2:
            return

        time_range = self._max_time - self._min_time
        mem_range = self._max_mem - self._min_mem

        if time_range <= 0 or mem_range <= 0:
            return

        # 转换坐标
        def to_x(t):
            return rect.left() + (t - self._min_time) / time_range * rect.width()

        def to_y(m):
            return rect.bottom() - (m - self._min_mem) / mem_range * rect.height()

        # 先绘制 RSS 曲线 (总内存，蓝色) - 在下层
        painter.setPen(QPen(get_color(ColorKey.CHART_LINE), 2))
        for i in range(1, len(self._data)):
            t1, _, rss1 = self._data[i - 1]
            t2, _, rss2 = self._data[i]
            painter.drawLine(int(to_x(t1)), int(to_y(rss1)), int(to_x(t2)), int(to_y(rss2)))

        # 再绘制 USS 曲线 (私有内存，橙色) - 在上层，确保可见
        painter.setPen(QPen(get_color(ColorKey.CHART_LINE_SECONDARY), 2))
        for i in range(1, len(self._data)):
            t1, uss1, _ = self._data[i - 1]
            t2, uss2, _ = self._data[i]
            painter.drawLine(int(to_x(t1)), int(to_y(uss1)), int(to_x(t2)), int(to_y(uss2)))

        # 绘制图例
        self._draw_legend(painter, rect)

    def _draw_legend(self, painter: QPainter, rect):
        """绘制图例"""
        painter.setFont(QFont("Microsoft YaHei", 8))
        legend_x = rect.right() - 100
        legend_y = rect.top() + 12

        # RSS 图例
        painter.setPen(QPen(get_color(ColorKey.CHART_LINE), 2))
        painter.drawLine(legend_x, legend_y, legend_x + 20, legend_y)
        painter.setPen(get_color(ColorKey.TEXT_SECONDARY))
        painter.drawText(legend_x + 25, legend_y + 4, "总占用")

        # USS 图例
        painter.setPen(QPen(get_color(ColorKey.CHART_LINE_SECONDARY), 2))
        painter.drawLine(legend_x, legend_y + 14, legend_x + 20, legend_y + 14)
        painter.setPen(get_color(ColorKey.TEXT_SECONDARY))
        painter.drawText(legend_x + 25, legend_y + 18, "私有")


class QObjectStatsWidget(QWidget):
    """QObject 统计组件"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 4, 0, 4)

        # 标题和刷新按钮
        header = QHBoxLayout()
        header.addWidget(QLabel("QObject 统计"))

        self.refresh_btn = QPushButton("刷新")
        self.refresh_btn.setFixedWidth(60)
        header.addStretch()
        header.addWidget(self.refresh_btn)
        layout.addLayout(header)

        # 统计结果
        self.stats_text = QTextEdit()
        self.stats_text.setReadOnly(True)
        self.stats_text.setPlaceholderText("点击刷新查看 QObject 统计")
        layout.addWidget(self.stats_text)

        # 连接信号
        self.refresh_btn.clicked.connect(self._refresh)

    def _refresh(self):
        """刷新 QObject 统计"""
        from PySide6.QtCore import QObject
        from PySide6.QtWidgets import QApplication

        # 从 QApplication 根节点获取所有 QObject
        app = QApplication.instance()
        if app is None:
            self.stats_text.setPlainText("QApplication 未初始化")
            return

        all_objects = app.findChildren(QObject, "")

        # 按类型统计
        type_counter = Counter(type(obj).__name__ for obj in all_objects)

        # 生成报告
        lines = [f"QObject 总数: {len(all_objects)}", ""]
        lines.append("按类型统计 (Top 30):")
        lines.append("-" * 40)
        for name, count in type_counter.most_common(30):
            lines.append(f"{name}: {count}")

        self.stats_text.setPlainText("\n".join(lines))


class MemorySnapshotWidget(QWidget):
    """内存快照组件"""

    def __init__(self, parent=None, auto_start: bool = False):
        super().__init__(parent)
        self._baseline: Optional[tracemalloc.Snapshot] = None
        self._auto_start = auto_start
        self._setup_ui()

        # 自动启动追踪
        if auto_start and tracemalloc.is_tracing():
            self._on_auto_started()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 4, 0, 4)

        # 标题
        header = QHBoxLayout()
        header.addWidget(QLabel("内存分析 (tracemalloc)"))
        layout.addLayout(header)

        # 按钮
        btn_layout = QHBoxLayout()

        self.start_btn = QPushButton("启动追踪")
        self.start_btn.setFixedWidth(80)
        btn_layout.addWidget(self.start_btn)

        self.memory_dist_btn = QPushButton("内存分布")
        self.memory_dist_btn.setFixedWidth(80)
        btn_layout.addWidget(self.memory_dist_btn)

        self.snapshot_btn = QPushButton("对比快照")
        self.snapshot_btn.setFixedWidth(80)
        btn_layout.addWidget(self.snapshot_btn)

        self.stop_btn = QPushButton("停止追踪")
        self.stop_btn.setFixedWidth(80)
        btn_layout.addWidget(self.stop_btn)

        btn_layout.addStretch()
        layout.addLayout(btn_layout)

        # 状态
        self.status_label = QLabel("状态: 未启动")
        layout.addWidget(self.status_label)

        # 结果
        self.result_text = QTextEdit()
        self.result_text.setReadOnly(True)
        self.result_text.setPlaceholderText("点击'内存分布'查看当前内存分配")
        layout.addWidget(self.result_text)

        # 连接信号
        self.start_btn.clicked.connect(self._start_tracing)
        self.memory_dist_btn.clicked.connect(self._show_memory_distribution)
        self.snapshot_btn.clicked.connect(self._take_snapshot)
        self.stop_btn.clicked.connect(self._stop_tracing)

        # 更新按钮状态
        self._update_button_states()

    def _update_button_states(self):
        """更新按钮状态"""
        is_tracing = tracemalloc.is_tracing()
        self.start_btn.setEnabled(not is_tracing)
        self.memory_dist_btn.setEnabled(is_tracing)
        self.snapshot_btn.setEnabled(is_tracing)
        self.stop_btn.setEnabled(is_tracing)

        if is_tracing:
            self.status_label.setText("状态: 追踪中")
        else:
            self.status_label.setText("状态: 未启动")

    def _on_auto_started(self):
        """自动启动后的状态更新"""
        self._update_button_states()
        self.status_label.setText("状态: 追踪中 (debug 模式自动启动)")

    def _start_tracing(self):
        """启动 tracemalloc"""
        if not tracemalloc.is_tracing():
            tracemalloc.start()
        self._baseline = tracemalloc.take_snapshot()
        self._update_button_states()

    def _show_memory_distribution(self):
        """显示当前内存分布"""
        if not tracemalloc.is_tracing():
            self.status_label.setText("状态: 未追踪,请先启动")
            return

        snapshot = tracemalloc.take_snapshot()
        stats = snapshot.statistics('lineno')

        # 计算总内存
        total_size = sum(stat.size for stat in stats)
        total_count = sum(stat.count for stat in stats)

        lines = [
            f"总内存: {total_size / 1024:.1f} KB ({total_count} 个分配)",
            "",
            "按文件分布 Top 30:",
            "-" * 80,
        ]

        for stat in stats[:30]:
            size_kb = stat.size / 1024
            lines.append(f"{size_kb:8.1f} KB  {stat.count:6d}x  {stat}")

        # 按文件汇总
        file_stats = {}
        for stat in stats:
            # 提取文件名
            if ':' in str(stat):
                filename = str(stat).split(':')[0].strip()
            else:
                filename = "unknown"
            if filename not in file_stats:
                file_stats[filename] = {"size": 0, "count": 0}
            file_stats[filename]["size"] += stat.size
            file_stats[filename]["count"] += stat.count

        lines.extend([
            "",
            "按文件汇总 Top 15:",
            "-" * 80,
        ])

        sorted_files = sorted(file_stats.items(), key=lambda x: x[1]["size"], reverse=True)
        for filename, data in sorted_files[:15]:
            size_kb = data["size"] / 1024
            pct = data["size"] / total_size * 100 if total_size > 0 else 0
            lines.append(f"{size_kb:8.1f} KB ({pct:5.1f}%)  {filename}")

        self.result_text.setPlainText("\n".join(lines))

    def _take_snapshot(self):
        """对比快照"""
        if not tracemalloc.is_tracing():
            self.status_label.setText("状态: 未追踪,请先启动")
            return

        current = tracemalloc.take_snapshot()
        if self._baseline:
            stats = current.compare_to(self._baseline, 'lineno')

            lines = ["内存分配变化 Top 20:", "-" * 60]
            for stat in stats[:20]:
                lines.append(str(stat))

            self.result_text.setPlainText("\n".join(lines))
        else:
            self.result_text.setPlainText("无基线快照")

        # 更新基线
        self._baseline = current

    def _stop_tracing(self):
        """停止追踪"""
        if tracemalloc.is_tracing():
            tracemalloc.stop()

        self._baseline = None
        self._update_button_states()

    def cleanup(self):
        """清理资源"""
        # 不在这里停止 tracemalloc，因为可能是 debug 模式自动启动的
        pass


class MemoryWindow(QWidget):
    """内存监控窗口"""

    def __init__(self, parent=None, auto_tracemalloc: bool = False):
        super().__init__(parent)
        self._start_time = 0.0
        self._auto_tracemalloc = auto_tracemalloc
        self._setup_ui()
        self._setup_timer()

    def _setup_ui(self):
        self.setWindowTitle("性能监控")
        self.setMinimumSize(600, 500)
        self.resize(800, 600)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)

        # 实时内存信息
        self.memory_label = QLabel("内存: -- MB")
        self.memory_label.setStyleSheet("font-size: 16px; font-weight: bold;")
        layout.addWidget(self.memory_label)

        # 分割器
        splitter = HighlightSplitter(Qt.Vertical)

        # 内存曲线图
        self.memory_chart = MemoryChart()
        splitter.addWidget(self.memory_chart)

        # 下方面板
        bottom_panel = QWidget()
        bottom_layout = QHBoxLayout(bottom_panel)
        bottom_layout.setContentsMargins(0, 0, 0, 0)

        # QObject 统计
        self.qobject_stats = QObjectStatsWidget()
        bottom_layout.addWidget(self.qobject_stats)

        # 内存快照 (传递 auto_tracemalloc 参数)
        self.memory_snapshot = MemorySnapshotWidget(auto_start=self._auto_tracemalloc)
        bottom_layout.addWidget(self.memory_snapshot)

        splitter.addWidget(bottom_panel)
        splitter.setSizes([300, 300])

        layout.addWidget(splitter)

    def _setup_timer(self):
        """设置定时器"""
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._update_memory)

    def _update_memory(self):
        """更新内存显示"""
        if not HAS_PSUTIL:
            self.memory_label.setText("内存: psutil 未安装")
            return

        process = psutil.Process(os.getpid())

        # 获取 RSS (总内存/工作集)
        rss_mb = process.memory_info().rss / 1024 / 1024

        # 获取 USS (私有内存)
        try:
            uss_mb = process.memory_full_info().uss / 1024 / 1024
        except Exception:
            uss_mb = rss_mb  # 回退

        elapsed = time.time() - self._start_time

        self.memory_label.setText(f"私有: {uss_mb:.1f} MB | 总计: {rss_mb:.1f} MB")
        self.memory_chart.add_point(elapsed, uss_mb, rss_mb)

    def showEvent(self, event):
        """窗口显示时启动监控"""
        super().showEvent(event)
        self._start_time = time.time()
        self.memory_chart.clear()
        self._timer.start(1000)  # 1秒刷新

    def hideEvent(self, event):
        """窗口隐藏时停止监控"""
        super().hideEvent(event)
        self._timer.stop()

    def closeEvent(self, event):
        """窗口关闭时清理资源"""
        self._timer.stop()
        self.memory_snapshot.cleanup()
        super().closeEvent(event)
