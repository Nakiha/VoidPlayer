"""
DiagnosticsManager - 诊断管理器

负责:
- 协调 PerformanceMonitor 和 StatsWindow
- 管理 Excel 导出
- 提供统一的诊断开关
"""
from datetime import datetime
from pathlib import Path
from typing import TYPE_CHECKING

from PySide6.QtCore import QObject, Signal

from player.core.diagnostics.performance_monitor import PerformanceMonitor, FrameTiming
from player.core.logging_config import get_logger

if TYPE_CHECKING:
    from player.core.decoder_pool import DecoderPool


class DiagnosticsManager(QObject):
    """
    诊断管理器

    功能:
    - 管理 PerformanceMonitor 实例
    - 控制性能统计 UI 显示
    - 导出诊断报告到 Excel

    信号:
        stats_updated: 统计数据更新 (dict)
        export_completed: 导出完成 (file_path)
    """

    # 信号
    stats_updated = Signal(dict)  # 转发 PerformanceMonitor 的统计数据
    export_completed = Signal(str)  # 导出完成

    def __init__(self, decoder_pool: "DecoderPool", parent=None):
        super().__init__(parent)
        self._decoder_pool = decoder_pool
        self._logger = get_logger()

        # 性能监控器
        self._perf_monitor = PerformanceMonitor(decoder_pool, self)
        self._perf_monitor.stats_updated.connect(self.stats_updated.emit)

        # Excel 导出
        self._export_dir = Path.cwd() / "diagnostics"
        self._frame_records: list[dict] = []  # 用于导出的帧记录

        # 连接帧时序记录
        self._perf_monitor.frame_timing_recorded.connect(self._on_frame_timing_recorded)

    # ========== 公共 API ==========

    def start(self):
        """开始诊断 (播放开始时调用)"""
        self._perf_monitor.start()
        # 清空之前的记录
        self._frame_records.clear()
        self._logger.info("[Diagnostics] Started recording frame timings")

    def stop(self):
        """停止诊断 (播放停止时调用)"""
        self._perf_monitor.stop()
        self._logger.info(f"[Diagnostics] Stopped, recorded {len(self._frame_records)} frames")

    def on_frame_requested(self, track_index: int):
        """帧请求事件"""
        self._perf_monitor.on_frame_requested(track_index)

    def on_frame_completed(self, track_index: int, pts_ms: int, success: bool):
        """帧完成事件"""
        self._perf_monitor.on_frame_completed(track_index, pts_ms, success)

    @property
    def perf_monitor(self) -> PerformanceMonitor:
        """获取性能监控器"""
        return self._perf_monitor

    # ========== Excel 导出 ==========

    def set_export_enabled(self, enabled: bool, export_dir: str | Path | None = None):
        """
        启用/禁用自动导出 (播放停止时自动导出)

        Args:
            enabled: 是否启用
            export_dir: 导出目录 (默认 diagnostics/)
        """
        if export_dir:
            self._export_dir = Path(export_dir)

        if enabled:
            self._logger.info(f"[Diagnostics] Auto export enabled, dir: {self._export_dir}")

    def export_now(self) -> str | None:
        """
        立即导出当前记录

        Returns:
            导出文件路径，失败返回 None
        """
        if not self._frame_records:
            self._logger.warning("[Diagnostics] No frame data to export")
            return None

        self._logger.info(f"[Diagnostics] Exporting {len(self._frame_records)} frames...")
        return self._export_to_file()

    # ========== 内部方法 ==========

    def _on_frame_timing_recorded(self, timing: FrameTiming):
        """记录帧时序 (用于导出)"""
        self._frame_records.append({
            "timestamp": datetime.now().isoformat(),
            "track_index": timing.track_index,
            "pts_ms": timing.pts_ms,
            "decode_time_ms": round(timing.decode_time_ms, 2),
            "request_time": timing.request_time,
            "complete_time": timing.complete_time,
        })

    def _export_to_file(self) -> str:
        """导出到文件 (Excel 或 CSV)"""
        try:
            import pandas as pd
            return self._export_to_excel(pd)
        except ImportError:
            self._logger.warning("[Diagnostics] pandas not available, falling back to CSV")
            return self._export_to_csv()

    def _export_to_excel(self, pd) -> str:
        """导出到 Excel"""
        self._export_dir.mkdir(parents=True, exist_ok=True)

        # 生成文件名
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        file_path = self._export_dir / f"diagnostics_{timestamp}.xlsx"

        # 创建帧时序 DataFrame
        df = pd.DataFrame(self._frame_records)

        # 添加统计汇总
        summary_data = []
        for track_idx, stats in self._perf_monitor._track_stats.items():
            summary_data.append({
                "track_index": track_idx,
                "total_frames": stats.total_frames,
                "late_frames": stats.late_frames,
                "late_frame_ratio": round(stats.late_frames / stats.total_frames, 4) if stats.total_frames > 0 else 0,
                "avg_decode_time_ms": round(stats.avg_decode_time_ms, 2),
                "max_decode_time_ms": round(stats.max_decode_time_ms, 2),
                "target_fps": round(stats.fps, 2),
                "actual_fps": round(stats.current_fps, 2),
                "is_bottleneck": stats.is_bottleneck,
            })

        summary_df = pd.DataFrame(summary_data)

        # 写入 Excel
        with pd.ExcelWriter(file_path, engine="openpyxl") as writer:
            df.to_excel(writer, sheet_name="帧时序", index=False)
            if not summary_df.empty:
                summary_df.to_excel(writer, sheet_name="统计汇总", index=False)

        self._logger.info(f"[Diagnostics] Exported to: {file_path}")
        self.export_completed.emit(str(file_path))
        return str(file_path)

    def _export_to_csv(self) -> str:
        """导出到 CSV (pandas 不可用时的降级方案)"""
        import csv

        self._export_dir.mkdir(parents=True, exist_ok=True)

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        file_path = self._export_dir / f"diagnostics_{timestamp}.csv"

        with open(file_path, "w", newline="", encoding="utf-8") as f:
            if self._frame_records:
                writer = csv.DictWriter(f, fieldnames=self._frame_records[0].keys())
                writer.writeheader()
                writer.writerows(self._frame_records)

        self._logger.info(f"[Diagnostics] Exported to: {file_path}")
        self.export_completed.emit(str(file_path))
        return str(file_path)
