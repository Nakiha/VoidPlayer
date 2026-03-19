"""
DecoderPoolAsync - 异步多轨道解码器池

替代原有的 DecoderPool，使用异步架构:
- 异步媒体探测
- 后台解码线程
- 可取消的精确 seek
- GL 线程纹理上传调度
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Callable
from PySide6.QtCore import QObject, Signal, QTimer

from player.native import voidview_native
from player.core.logging_config import get_logger
from player.core.async_manager import AsyncOperationManager
from player.core.decode_worker import DecodeWorker, DecodeWorkerPool, CommandType, DecodeCommand
from player.core.texture_upload_scheduler import TextureUploadScheduler

if TYPE_CHECKING:
    pass


@dataclass
class MediaInfo:
    """媒体信息数据类"""
    path: str
    valid: bool = False
    width: int = 0
    height: int = 0
    duration_ms: int = 0
    fps: float = 0.0
    codec_name: str = ""
    format_name: str = ""
    seekable: bool = True
    has_audio: bool = False
    error_message: str = ""

    @classmethod
    def from_native(cls, path: str, native_info) -> "MediaInfo":
        """从 voidview_native.MediaInfo 创建"""
        return cls(
            path=path,
            valid=native_info.valid,
            width=native_info.width,
            height=native_info.height,
            duration_ms=native_info.duration_ms,
            fps=native_info.fps,
            codec_name=native_info.codec_name,
            format_name=native_info.format_name,
            seekable=native_info.seekable,
            has_audio=native_info.has_audio,
            error_message=native_info.error_message,
        )

    @property
    def aspect_ratio(self) -> float:
        """宽高比"""
        if self.height > 0:
            return self.width / self.height
        return 16 / 9


@dataclass
class TrackState:
    """单个轨道的状态"""
    index: int
    path: str
    media_info: MediaInfo | None = None
    decoder: voidview_native.HardwareDecoder | None = None
    worker: DecodeWorker | None = None
    offset_ms: int = 0  # 同步偏移
    enabled: bool = True  # 是否显示
    current_pts_ms: int = 0

    @property
    def texture_id(self) -> int:
        """当前纹理 ID"""
        if self.worker:
            return self.worker.get_texture_id()
        return 0


class DecoderPoolAsync(QObject):
    """
    异步多轨道解码器池

    特性:
    - 异步媒体探测 (不阻塞 UI)
    - 后台解码线程 (每个轨道一个)
    - 可取消的精确 seek
    - GL 纹理上传调度

    信号:
        frame_ready: 所有活动轨道解码完一帧 (纹理已上传)
        duration_changed: 总时长变化
        position_changed: 播放位置变化 (ms)
        eof_reached: 到达文件末尾
        error_occurred: 发生错误 (index, message)
        seek_started: seek 开始 (target_ms)
        seek_completed: seek 完成 (actual_ms)
        seek_cancelled: seek 被取消
        track_initialized: 轨道解码器初始化完成 (index)
    """

    MAX_TRACKS = 8

    # 信号
    frame_ready = Signal()
    duration_changed = Signal(int)
    position_changed = Signal(int)
    eof_reached = Signal()
    error_occurred = Signal(int, str)
    seek_started = Signal(int)
    seek_completed = Signal(int)
    seek_cancelled = Signal()
    track_initialized = Signal(int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._logger = get_logger()

        # 轨道状态
        self._tracks: list[TrackState | None] = [None] * self.MAX_TRACKS
        self._track_count = 0

        # 播放状态
        self._is_playing = False
        self._current_position_ms = 0
        self._duration_ms = 0

        # 异步组件
        self._async_manager = AsyncOperationManager(self)
        self._worker_pool = DecodeWorkerPool()
        self._upload_scheduler = TextureUploadScheduler(self)

        # 播放定时器
        self._play_timer = QTimer(self)
        self._play_timer.timeout.connect(self._on_play_timer)
        self._frame_interval_ms = 16  # ~60fps

        # pending seek (用于合并快速连续 seek)
        self._pending_seek_ms: int | None = None
        self._pending_seek_precise: bool = False
        self._seek_timer = QTimer(self)
        self._seek_timer.setSingleShot(True)
        self._seek_timer.timeout.connect(self._execute_pending_seek)

        # 当前 seek 操作计数 (用于判断是否所有轨道都完成了)
        self._pending_seek_count = 0
        self._seek_target_ms = 0

        # 连接信号
        self._setup_signal_connections()

    def _setup_signal_connections(self):
        """设置信号连接"""
        # Worker 信号
        for i in range(self.MAX_TRACKS):
            worker = self._worker_pool.get_worker(i)
            if worker:
                self._connect_worker_signals(worker, i)

        # 上传调度器信号
        self._upload_scheduler.upload_completed.connect(self._on_upload_completed)

    def _connect_worker_signals(self, worker: DecodeWorker, track_index: int):
        """连接 worker 信号"""
        worker.frame_upload_ready.connect(self._upload_scheduler.schedule_upload)
        worker.frame_decoded.connect(lambda idx: self._on_frame_decoded(idx))
        worker.seek_completed.connect(lambda idx, pos: self._on_seek_completed(idx, pos))
        worker.seek_cancelled.connect(lambda idx: self._on_seek_cancelled(idx))
        worker.error_occurred.connect(lambda idx, msg: self.error_occurred.emit(idx, msg))

    # ========== 异步媒体探测 ==========

    def probe_file_async(self, path: str, callback: Callable[[MediaInfo], None]) -> int:
        """
        异步探测媒体文件

        Args:
            path: 文件路径
            callback: 完成回调

        Returns:
            操作 ID
        """
        def probe_task():
            native_info = voidview_native.probe_file(path)
            return MediaInfo.from_native(path, native_info)

        def on_result(result):
            if result.state.name == 'COMPLETED' and result.result:
                callback(result.result)
            else:
                callback(MediaInfo(path=path, valid=False, error_message=result.error or "Unknown error"))

        return self._async_manager.submit(probe_task, callback=on_result)

    @staticmethod
    def probe_file(path: str) -> MediaInfo:
        """同步探测媒体文件 (保留兼容性)"""
        native_info = voidview_native.probe_file(path)
        return MediaInfo.from_native(path, native_info)

    # ========== 轨道管理 ==========

    def add_track(self, index: int, path: str) -> bool:
        """
        添加轨道

        Args:
            index: 轨道索引 (0-7)
            path: 文件路径

        Returns:
            True 如果成功
        """
        if not (0 <= index < self.MAX_TRACKS):
            return False

        if self._tracks[index] is not None:
            self.remove_track(index)

        # 同步探测 (添加轨道时通常可以接受短暂阻塞)
        media_info = self.probe_file(path)
        if not media_info.valid:
            self.error_occurred.emit(index, media_info.error_message)
            return False

        # 创建轨道状态
        track = TrackState(index=index, path=path, media_info=media_info)
        self._tracks[index] = track
        self._track_count = sum(1 for t in self._tracks if t is not None)

        # 更新总时长
        self._update_duration()

        return True

    def remove_track(self, index: int):
        """移除轨道"""
        if not (0 <= index < self.MAX_TRACKS):
            return

        track = self._tracks[index]
        if track:
            # 停止 worker
            if track.worker:
                self._worker_pool.remove_worker(index)

        self._tracks[index] = None
        self._track_count = sum(1 for t in self._tracks if t is not None)
        self._update_duration()

    def initialize_decoder_async(self, index: int) -> bool:
        """
        异步初始化解码器

        注意: 必须在有 OpenGL 上下文的线程中调用

        Args:
            index: 轨道索引

        Returns:
            True 如果初始化成功或已在初始化中
        """
        if not (0 <= index < self.MAX_TRACKS):
            return False

        track = self._tracks[index]
        if not track or not track.media_info:
            return False

        if track.decoder:
            return True  # 已初始化

        try:
            # 创建解码器
            decoder = voidview_native.HardwareDecoder(track.path)
            if not decoder.initialize(0):
                self.error_occurred.emit(index, decoder.error_message)
                return False

            # 设置 OpenGL 上下文
            decoder.set_opengl_context(0)

            # 创建 worker
            worker = self._worker_pool.create_worker(index)
            worker.set_decoder(decoder)

            # 连接信号
            self._connect_worker_signals(worker, index)

            # 更新轨道状态
            track.decoder = decoder
            track.worker = worker

            # 更新帧间隔
            if track.media_info.fps > 0:
                self._frame_interval_ms = int(1000 / track.media_info.fps)

            # 设置上传调度器
            self._upload_scheduler.set_worker(index, worker)

            self.track_initialized.emit(index)
            return True

        except Exception as e:
            self.error_occurred.emit(index, str(e))
            return False

    def get_decoders(self) -> list[voidview_native.HardwareDecoder | None]:
        """获取所有解码器列表 (用于绑定到 GLWidget)"""
        return [t.decoder if t else None for t in self._tracks]

    def get_track_state(self, index: int) -> TrackState | None:
        """获取轨道状态"""
        if 0 <= index < self.MAX_TRACKS:
            return self._tracks[index]
        return None

    def set_offset(self, index: int, offset_ms: int):
        """设置轨道时间偏移"""
        track = self.get_track_state(index)
        if track:
            track.offset_ms = offset_ms

    def set_enabled(self, index: int, enabled: bool):
        """设置轨道是否启用"""
        track = self.get_track_state(index)
        if track:
            track.enabled = enabled

    # ========== 播放控制 ==========

    def play(self):
        """开始播放"""
        if self._is_playing:
            return

        self._is_playing = True
        self._play_timer.start(self._frame_interval_ms)
        self._logger.debug("Playback started")

    def pause(self):
        """暂停播放"""
        self._is_playing = False
        self._play_timer.stop()
        self._logger.debug("Playback paused")

    def toggle_play(self):
        """切换播放/暂停"""
        if self._is_playing:
            self.pause()
        else:
            self.play()

    def seek_to(self, position_ms: int):
        """
        快速跳转到指定时间 (关键帧级别)

        使用延迟执行策略合并快速连续 seek。
        """
        position_ms = max(0, min(position_ms, self._duration_ms))
        self._schedule_seek(position_ms, is_precise=False)

    def seek_to_precise(self, position_ms: int):
        """
        精确跳转到指定时间 (帧级别)

        使用延迟执行策略合并快速连续 seek。
        支持取消。
        """
        position_ms = max(0, min(position_ms, self._duration_ms))
        self._schedule_seek(position_ms, is_precise=True)

    def _schedule_seek(self, position_ms: int, is_precise: bool):
        """调度延迟 seek"""
        self._pending_seek_ms = position_ms
        self._pending_seek_precise = is_precise
        self._seek_timer.start(50)  # 50ms 延迟

    def cancel_seek(self):
        """取消当前 seek 操作"""
        self._worker_pool.cancel_all_seeks()
        self._pending_seek_count = 0
        self.seek_cancelled.emit()

    def _execute_pending_seek(self):
        """执行挂起的 seek"""
        if self._pending_seek_ms is None:
            return

        target_ms = self._pending_seek_ms
        is_precise = self._pending_seek_precise
        self._pending_seek_ms = None

        self._logger.debug(f"Executing seek to {target_ms}ms (precise={is_precise})")

        # 取消之前的 seek
        self._worker_pool.cancel_all_seeks()

        # 重置计数
        self._pending_seek_count = 0
        self._seek_target_ms = target_ms

        # 广播 seek
        for track in self._tracks:
            if not track or not track.worker or not track.enabled:
                continue

            adjusted_ms = max(0, target_ms + track.offset_ms)
            cmd_type = CommandType.SEEK_PRECISE if is_precise else CommandType.SEEK

            def on_seek_done(success, pts_ms, t=track):
                if success and pts_ms is not None:
                    t.current_pts_ms = pts_ms

            track.worker.post_command(DecodeCommand(cmd_type, adjusted_ms, on_seek_done))
            self._pending_seek_count += 1

        if self._pending_seek_count > 0:
            self.seek_started.emit(target_ms)

    # ========== 属性 ==========

    @property
    def is_playing(self) -> bool:
        return self._is_playing

    @property
    def current_position_ms(self) -> int:
        return self._current_position_ms

    position_ms = current_position_ms  # 别名

    @property
    def duration_ms(self) -> int:
        return self._duration_ms

    @property
    def track_count(self) -> int:
        return self._track_count

    @property
    def upload_scheduler(self) -> TextureUploadScheduler:
        return self._upload_scheduler

    # ========== 内部方法 ==========

    def _on_play_timer(self):
        """播放定时器回调"""
        if not self._is_playing:
            return

        # 向所有活动轨道发送解码命令
        any_active = False
        for track in self._tracks:
            if not track or not track.enabled or not track.worker:
                continue
            if track.decoder and track.decoder.eof:
                continue

            any_active = True
            track.worker.post_command(DecodeCommand(CommandType.DECODE))

        # 检查 EOF
        if not any_active:
            self.pause()
            self.eof_reached.emit()
            return

        # 处理待上传帧
        self._upload_scheduler.process_pending_uploads()

    def _on_frame_decoded(self, track_index: int):
        """帧解码完成回调"""
        track = self._tracks[track_index]
        if track and track.worker:
            track.current_pts_ms = track.worker.get_current_pts_ms()

    def _on_seek_completed(self, track_index: int, position_ms: int):
        """seek 完成回调"""
        self._pending_seek_count -= 1
        self._logger.trace(f"Seek completed for track {track_index} at {position_ms}ms, pending={self._pending_seek_count}")

        if self._pending_seek_count <= 0:
            self._pending_seek_count = 0
            # 更新当前位置
            self._current_position_ms = self._seek_target_ms
            self.position_changed.emit(self._current_position_ms)
            self.seek_completed.emit(position_ms)

            # 处理待上传帧
            self._upload_scheduler.process_pending_uploads()

    def _on_seek_cancelled(self, track_index: int):
        """seek 被取消回调"""
        self._pending_seek_count -= 1
        self._logger.trace(f"Seek cancelled for track {track_index}, pending={self._pending_seek_count}")

    def _on_upload_completed(self, track_index: int):
        """纹理上传完成回调"""
        # 检查所有活动轨道是否都已上传
        all_ready = True
        for track in self._tracks:
            if track and track.enabled and track.worker:
                if not track.worker.has_pending_frame():
                    # 还没有新帧，可能还在解码中
                    pass

        self.frame_ready.emit()

    def _update_duration(self):
        """更新总时长"""
        max_duration = 0
        for track in self._tracks:
            if track and track.media_info:
                duration = track.media_info.duration_ms - track.offset_ms
                max_duration = max(max_duration, duration)

        if max_duration != self._duration_ms:
            self._duration_ms = max_duration
            self.duration_changed.emit(self._duration_ms)

    def process_pending_uploads(self) -> int:
        """
        处理待上传帧 (由 GLWidget 在 paintGL 前调用)

        Returns:
            成功上传的帧数
        """
        return self._upload_scheduler.process_pending_uploads()

    def clear(self):
        """清除所有轨道"""
        self.pause()
        self._worker_pool.stop_all()
        self._tracks = [None] * self.MAX_TRACKS
        self._track_count = 0
        self._current_position_ms = 0
        self._duration_ms = 0
        self._upload_scheduler.clear_pending()
        self.duration_changed.emit(0)

    def shutdown(self):
        """关闭解码器池"""
        self.pause()
        self._worker_pool.stop_all()
        self._async_manager.shutdown()
        self._logger.debug("DecoderPoolAsync shutdown")
