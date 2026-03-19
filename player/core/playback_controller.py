"""
PlaybackController - 播放节奏控制器

负责:
- 统一的多轨道播放节奏控制
- 基于主时钟的帧调度
- 支持不同帧率的视频同步播放
- 播放速度控制

核心设计:
- 主时钟以固定频率 (60Hz) 运行
- 根据主时钟计算当前应该播放的时间位置
- 请求每个轨道解码到对应时间位置的帧

性能监控由 DiagnosticsManager 独立处理
"""
import time
from typing import TYPE_CHECKING

from PySide6.QtCore import QObject, QTimer, Signal

from player.core.logging_config import get_logger

if TYPE_CHECKING:
    from player.core.decoder_pool import DecoderPool, TrackState


class PlaybackController(QObject):
    """
    播放节奏控制器

    使用主时钟驱动多轨道同步播放:
    - 主时钟频率: 60Hz (约 16.67ms)
    - 根据经过的时间计算播放位置
    - 请求每个轨道解码到对应位置的帧

    信号:
        frame_tick: 每个主时钟周期发射，通知 UI 更新
        frame_requested: 帧请求事件 (track_index) - 供诊断模块订阅
        frame_completed: 帧完成事件 (track_index, pts_ms, success) - 供诊断模块订阅
    """

    # 主时钟频率 (ms) - 使用 60Hz 作为基准
    MASTER_CLOCK_INTERVAL_MS = 16

    # 信号
    frame_tick = Signal()  # 每帧周期通知
    frame_requested = Signal(int)  # 帧请求事件 (track_index)
    frame_completed = Signal(int, int, bool)  # 帧完成事件 (track_index, pts_ms, success)

    def __init__(self, decoder_pool: "DecoderPool", parent=None):
        super().__init__(parent)
        self._decoder_pool = decoder_pool
        self._logger = get_logger()

        # 播放状态
        self._is_playing = False
        self._playback_speed = 1.0  # 播放速度倍率

        # 主时钟
        self._master_timer = QTimer(self)
        self._master_timer.timeout.connect(self._on_master_tick)

        # 播放开始时的系统时间 (用于计算主时钟)
        self._play_start_time_ms = 0

        # 播放开始时的轨道起始位置
        self._play_start_position_ms = 0

    # ========== 公共 API ==========

    def play(self):
        """开始播放"""
        self._logger.info(f"[Playback] play() called, current _is_playing={self._is_playing}")

        if self._is_playing:
            self._logger.warning("[Playback] play() called but already playing, ignoring")
            return

        self._is_playing = True

        # 记录播放开始时间
        self._play_start_time_ms = int(time.perf_counter() * 1000)
        self._play_start_position_ms = self._decoder_pool.current_position_ms

        # 清除所有轨道的 pending frame 状态
        # (暂停时可能遗留 pending frame 但回调被取消，导致状态不一致)
        self._clear_pending_frames()

        # 请求第一帧
        self._request_all_frames()

        # 启动主时钟
        self._master_timer.start(self.MASTER_CLOCK_INTERVAL_MS)
        self._logger.info(f"[Playback] Timer started, interval={self.MASTER_CLOCK_INTERVAL_MS}ms, isActive={self._master_timer.isActive()}")

        self._logger.info(f"[Playback] Started at position {self._play_start_position_ms}ms")

    def pause(self):
        """暂停播放"""
        self._logger.info(f"[Playback] pause() called, current _is_playing={self._is_playing}")

        if not self._is_playing:
            self._logger.warning("[Playback] pause() called but not playing, ignoring")
            return

        self._is_playing = False
        self._master_timer.stop()

        # 取消所有正在进行的解码
        for i in range(self._decoder_pool.MAX_TRACKS):
            track = self._decoder_pool.get_track_state(i)
            if track and track.worker:
                track.worker.cancel()

        self._logger.info(f"[Playback] Paused at position {self._decoder_pool.current_position_ms}ms")

    def seek_to(self, position_ms: int):
        """
        Seek 到指定位置

        暂停播放，执行 seek，然后根据之前是否在播放决定是否恢复
        """
        was_playing = self._is_playing

        # 暂停
        if was_playing:
            self._master_timer.stop()

        # 执行 seek
        self._decoder_pool.seek_to(position_ms)

        # 如果之前在播放，seek 完成后恢复
        if was_playing:
            # seek_completed 信号会触发 _on_seek_completed
            pass

    def set_speed(self, speed: float):
        """设置播放速度 (0.25 - 4.0)"""
        self._playback_speed = max(0.25, min(4.0, speed))

    @property
    def is_playing(self) -> bool:
        return self._is_playing

    @property
    def playback_speed(self) -> float:
        return self._playback_speed

    # ========== 内部方法 ==========

    def _clear_pending_frames(self):
        """清除所有轨道的 pending frame 状态"""
        for i in range(self._decoder_pool.MAX_TRACKS):
            track = self._decoder_pool.get_track_state(i)
            if track and track.decoder and track.decoder.has_pending_frame():
                # 上传并清除 pending frame
                track.decoder.upload_pending_frame()
                self._logger.debug(f"[Playback] Cleared pending frame for track {i}")

    def _on_master_tick(self):
        """主时钟回调 - 核心调度逻辑"""
        if not self._is_playing:
            self._logger.debug("[Playback] Master tick but not playing, stopping timer")
            self._master_timer.stop()
            return

        # 计算经过的时间 (考虑速度)
        now_ms = int(time.perf_counter() * 1000)
        elapsed_ms = (now_ms - self._play_start_time_ms) * self._playback_speed

        # 计算当前应该播放到的位置
        current_target_position = self._play_start_position_ms + elapsed_ms

        # 检查是否到达结束
        if current_target_position >= self._decoder_pool.duration_ms:
            self._on_all_tracks_finished()
            return

        # 更新播放位置
        self._decoder_pool._current_position_ms = int(current_target_position)

        # 请求帧
        self._request_all_frames()

        # 通知 UI 更新
        self.frame_tick.emit()

    def _request_all_frames(self):
        """为所有活动轨道请求解码下一帧"""
        requested_count = 0
        skipped_count = 0

        # 计算当前应该播放到的位置
        now_ms = int(time.perf_counter() * 1000)
        elapsed_ms = (now_ms - self._play_start_time_ms) * self._playback_speed
        current_target_position = self._play_start_position_ms + elapsed_ms

        for i in range(self._decoder_pool.MAX_TRACKS):
            track = self._decoder_pool.get_track_state(i)
            if not track or not track.enabled or not track.decoder or not track.worker:
                continue

            if track.decoder.eof:
                skipped_count += 1
                continue

            # 如果有正在等待的帧，跳过
            if track.decoder.has_pending_frame():
                skipped_count += 1
                continue

            # 计算该轨道应该播放到的位置 (考虑偏移)
            track_target = current_target_position + track.offset_ms

            # 检查当前帧的 PTS 是否已经足够新
            current_pts = track.current_pts_ms
            frame_interval = 16  # 默认 60fps
            if track.media_info and track.media_info.fps > 0:
                frame_interval = int(1000 / track.media_info.fps)

            # 如果当前帧的 PTS 已经 >= 目标位置，不需要请求新帧
            if current_pts >= track_target - frame_interval:
                skipped_count += 1
                continue

            # 请求解码下一帧
            track.worker.decode_frame()
            requested_count += 1

            # 发送帧请求信号 (供诊断模块订阅)
            self.frame_requested.emit(i)

        if requested_count > 0 or skipped_count > 0:
            self._logger.debug(f"[Playback] Frame request: {requested_count} requested, {skipped_count} skipped, target={current_target_position:.0f}ms")

    def _on_all_tracks_finished(self):
        """所有轨道都播放完成"""
        self._logger.info("[Playback] All tracks finished")
        self.pause()
        self._decoder_pool.eof_reached.emit()

    def _on_seek_completed(self, position_ms: int):
        """Seek 完成回调"""
        # 如果之前在播放，恢复播放
        if self._is_playing:
            # 重新初始化时间
            self._play_start_position_ms = position_ms
            self._play_start_time_ms = int(time.perf_counter() * 1000)

            # 请求帧
            self._request_all_frames()

            # 重启定时器
            self._master_timer.start(self.MASTER_CLOCK_INTERVAL_MS)

    def on_frame_decoded(self, track_index: int, pts_ms: int, success: bool):
        """帧解码完成回调 (由 DecoderPool 调用)"""
        # 发送帧完成信号 (供诊断模块订阅)
        self.frame_completed.emit(track_index, pts_ms, success)

    # ========== DecoderPool 信号连接 ==========

    def connect_to_decoder_pool(self):
        """连接 DecoderPool 信号"""
        self._decoder_pool.seek_completed.connect(self._on_seek_completed)
