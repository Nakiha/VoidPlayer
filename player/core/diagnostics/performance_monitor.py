"""
PerformanceMonitor - 播放性能监控器

负责:
- 追踪解码帧时序
- 检测解码性能瓶颈
- 统计帧丢弃和延迟
- 提供性能警告

参考 mpv 的 stats.lua 实现
"""
import time
from collections import deque
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from PySide6.QtCore import QObject, Signal, QTimer

from player.core.logging_config import get_logger

if TYPE_CHECKING:
    from player.core.decoder_pool import DecoderPool


@dataclass
class FrameTiming:
    """单帧时序信息"""
    request_time: float  # 请求解码的时间 (perf_counter)
    complete_time: float  # 解码完成的时间 (perf_counter)
    pts_ms: int  # 帧的 PTS
    track_index: int  # 轨道索引

    @property
    def decode_time_ms(self) -> float:
        """解码耗时 (ms)"""
        return (self.complete_time - self.request_time) * 1000


@dataclass
class TrackPerformance:
    """单个轨道的性能统计"""
    track_index: int
    fps: float = 0.0  # 视频帧率
    frame_interval_ms: float = 16.67  # 帧间隔 (ms)

    # 时序统计 (最近 N 帧)
    frame_timings: deque = field(default_factory=lambda: deque(maxlen=60))

    # 统计数据
    total_frames: int = 0
    dropped_frames: int = 0  # 因解码太慢而丢弃的帧
    late_frames: int = 0  # 延迟到达的帧 (超过帧间隔 150%)

    # 性能指标
    avg_decode_time_ms: float = 0.0
    max_decode_time_ms: float = 0.0
    current_fps: float = 0.0  # 实际帧率

    # 性能状态
    is_bottleneck: bool = False  # 是否存在性能瓶颈
    bottleneck_severity: float = 0.0  # 瓶颈严重程度 (0-1)

    def to_dict(self) -> dict:
        """转换为字典 (用于导出/UI)"""
        return {
            "fps": self.fps,
            "frame_interval_ms": self.frame_interval_ms,
            "total_frames": self.total_frames,
            "dropped_frames": self.dropped_frames,
            "late_frames": self.late_frames,
            "avg_decode_time_ms": self.avg_decode_time_ms,
            "max_decode_time_ms": self.max_decode_time_ms,
            "current_fps": self.current_fps,
            "is_bottleneck": self.is_bottleneck,
            "bottleneck_severity": self.bottleneck_severity,
        }


class PerformanceMonitor(QObject):
    """
    播放性能监控器

    追踪每个轨道的解码性能，检测瓶颈

    信号:
        frame_timing_recorded: 单帧时序记录 (FrameTiming)
        performance_warning: 性能警告 (track_index, message, severity)
        stats_updated: 统计数据更新 (dict)
    """

    # 性能阈值
    WARNING_THRESHOLD_RATIO = 0.8  # 解码时间超过帧间隔的 80% 发出警告
    CRITICAL_THRESHOLD_RATIO = 1.0  # 解码时间超过帧间隔 = 严重瓶颈
    LATE_FRAME_RATIO = 1.5  # 超过帧间隔 150% = 延迟帧

    # 统计周期 (ms)
    STATS_UPDATE_INTERVAL = 1000  # 每秒更新一次统计

    # 信号
    frame_timing_recorded = Signal(object)  # FrameTiming
    performance_warning = Signal(int, str, float)  # (track_index, message, severity)
    stats_updated = Signal(dict)  # 完整的统计数据

    def __init__(self, decoder_pool: "DecoderPool", parent=None):
        super().__init__(parent)
        self._decoder_pool = decoder_pool
        self._logger = get_logger()

        # 每个轨道的性能统计
        self._track_stats: dict[int, TrackPerformance] = {}

        # 正在进行的帧请求 (track_index -> request_time)
        self._pending_requests: dict[int, float] = {}

        # 主时钟开始时间
        self._playback_start_time: float = 0

        # 统计更新定时器
        self._stats_timer = QTimer(self)
        self._stats_timer.timeout.connect(self._update_stats)

        # 性能警告冷却 (避免频繁警告)
        self._warning_cooldown: dict[int, float] = {}
        self._warning_cooldown_seconds = 5.0

    # ========== 公共 API ==========

    def start(self):
        """开始监控"""
        self._playback_start_time = time.perf_counter()
        self._track_stats.clear()
        self._pending_requests.clear()
        self._warning_cooldown.clear()

        # 初始化轨道统计
        for i in range(self._decoder_pool.MAX_TRACKS):
            track = self._decoder_pool.get_track_state(i)
            if track and track.media_info:
                self._init_track_stats(i, track.media_info.fps)

        self._stats_timer.start(self.STATS_UPDATE_INTERVAL)
        self._logger.info("[PerfMon] Performance monitoring started")

    def stop(self):
        """停止监控"""
        self._stats_timer.stop()

    def on_frame_requested(self, track_index: int):
        """记录帧请求"""
        self._pending_requests[track_index] = time.perf_counter()

    def on_frame_completed(self, track_index: int, pts_ms: int, success: bool):
        """记录帧完成"""
        if track_index not in self._pending_requests:
            return

        request_time = self._pending_requests.pop(track_index)
        complete_time = time.perf_counter()

        if track_index not in self._track_stats:
            return

        stats = self._track_stats[track_index]

        if success:
            timing = FrameTiming(
                request_time=request_time,
                complete_time=complete_time,
                pts_ms=pts_ms,
                track_index=track_index
            )
            stats.frame_timings.append(timing)
            stats.total_frames += 1

            # 检查延迟帧
            decode_time = timing.decode_time_ms
            if decode_time > stats.frame_interval_ms * self.LATE_FRAME_RATIO:
                stats.late_frames += 1

            # 更新最大解码时间
            stats.max_decode_time_ms = max(stats.max_decode_time_ms, decode_time)

            # 发送单帧时序记录 (用于 Excel 导出)
            self.frame_timing_recorded.emit(timing)

    def get_track_stats(self, track_index: int) -> TrackPerformance | None:
        """获取轨道统计"""
        return self._track_stats.get(track_index)

    def get_all_stats(self) -> dict:
        """获取所有统计数据"""
        return {
            "tracks": {
                idx: stats.to_dict()
                for idx, stats in self._track_stats.items()
            },
            "playback_time": time.perf_counter() - self._playback_start_time,
        }

    def get_raw_timings(self) -> list[FrameTiming]:
        """获取所有原始时序数据 (用于导出)"""
        timings = []
        for stats in self._track_stats.values():
            timings.extend(stats.frame_timings)
        return sorted(timings, key=lambda t: t.request_time)

    # ========== 内部方法 ==========

    def _init_track_stats(self, track_index: int, fps: float):
        """初始化轨道统计"""
        if fps <= 0:
            fps = 30.0  # 默认 30fps

        self._track_stats[track_index] = TrackPerformance(
            track_index=track_index,
            fps=fps,
            frame_interval_ms=1000.0 / fps
        )

    def _update_stats(self):
        """更新统计数据 (定时调用)"""
        for track_index, stats in self._track_stats.items():
            if not stats.frame_timings:
                continue

            # 计算平均解码时间
            if stats.frame_timings:
                total_decode_time = sum(t.decode_time_ms for t in stats.frame_timings)
                stats.avg_decode_time_ms = total_decode_time / len(stats.frame_timings)

                # 计算实际帧率
                if len(stats.frame_timings) >= 2:
                    time_span = stats.frame_timings[-1].complete_time - stats.frame_timings[0].complete_time
                    if time_span > 0:
                        stats.current_fps = (len(stats.frame_timings) - 1) / time_span

            # 检测性能瓶颈
            self._check_bottleneck(track_index, stats)

        # 发送统计数据
        self.stats_updated.emit(self.get_all_stats())

    def _check_bottleneck(self, track_index: int, stats: TrackPerformance):
        """检测性能瓶颈"""
        if stats.avg_decode_time_ms <= 0:
            stats.is_bottleneck = False
            stats.bottleneck_severity = 0.0
            return

        # 计算瓶颈严重程度
        ratio = stats.avg_decode_time_ms / stats.frame_interval_ms

        if ratio >= self.CRITICAL_THRESHOLD_RATIO:
            stats.is_bottleneck = True
            stats.bottleneck_severity = min(1.0, ratio / 2.0)  # 严重
            self._emit_warning(track_index, "critical", ratio)
        elif ratio >= self.WARNING_THRESHOLD_RATIO:
            stats.is_bottleneck = True
            stats.bottleneck_severity = (ratio - self.WARNING_THRESHOLD_RATIO) / (
                self.CRITICAL_THRESHOLD_RATIO - self.WARNING_THRESHOLD_RATIO
            )
            self._emit_warning(track_index, "warning", ratio)
        else:
            stats.is_bottleneck = False
            stats.bottleneck_severity = 0.0

    def _emit_warning(self, track_index: int, level: str, ratio: float):
        """发出性能警告"""
        # 检查冷却
        now = time.perf_counter()
        last_warning = self._warning_cooldown.get(track_index, 0)
        if now - last_warning < self._warning_cooldown_seconds:
            return

        self._warning_cooldown[track_index] = now

        stats = self._track_stats.get(track_index)
        if not stats:
            return

        if level == "critical":
            message = (
                f"轨道 {track_index} 解码严重滞后! "
                f"解码耗时 {stats.avg_decode_time_ms:.1f}ms 超过帧间隔 {stats.frame_interval_ms:.1f}ms。"
                f"建议关闭其他占用 GPU/CPU 的程序。"
            )
        else:
            message = (
                f"轨道 {track_index} 解码性能接近瓶颈 "
                f"({ratio:.0%} 帧间隔)。如果出现卡顿，请关闭其他程序。"
            )

        self._logger.warning(f"[PerfMon] {message}")
        self.performance_warning.emit(track_index, message, stats.bottleneck_severity)
