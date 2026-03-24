"""
DecoderPool - 多轨道解码器管理器

负责:
- 管理多个 HardwareDecoder 实例
- 提供媒体信息探测 (probe_file)
- 异步解码 (C++ 后台线程，不阻塞 UI)
- 播放控制 (播放/暂停/seek) - 异步非阻塞

使用 DecodeWorker (C++ 独立线程) 实现非阻塞解码：
- DecodeWorker 在 C++ 层创建独立线程，不持有 GIL
- Python 只负责发命令、收结果、上传纹理 (在 GL 线程)
"""
import time
from dataclasses import dataclass
from typing import TYPE_CHECKING

from PySide6.QtCore import QObject, Signal, QMetaObject, Qt, Q_ARG, Slot

from player.native import voidview_native
from player.core.logging_config import get_logger

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
    worker: voidview_native.DecodeWorker | None = None  # C++ 后台解码线程
    offset_ms: int = 0  # 同步偏移
    enabled: bool = True  # 是否显示
    current_pts_ms: int = 0

    @property
    def texture_id(self) -> int:
        """当前纹理 ID"""
        if self.decoder:
            return self.decoder.texture_id
        return 0


class DecoderPool(QObject):
    """
    多轨道解码器池

    使用 C++ DecodeWorker 实现非阻塞解码：
    - DecodeWorker 在 C++ 层创建独立线程，不涉及 Python GIL
    - Python 主线程只负责：发命令、收结果、上传纹理
    - UI 不会被解码操作阻塞

    播放控制由 PlaybackController 负责，DecoderPool 只提供:
    - 解码器管理
    - 单帧解码请求
    - Seek 操作

    信号:
        frame_ready: 所有活动轨道解码完一帧
        duration_changed: 总时长变化
        position_changed: 播放位置变化 (ms)
        eof_reached: 到达文件末尾
        error_occurred: 发生错误 (index, message)
        seek_completed: seek 操作完成 (position_ms)
        track_frame_decoded: 单个轨道帧解码完成 (track_index, pts_ms, success)
    """

    MAX_TRACKS = 8

    # 信号
    frame_ready = Signal()  # 所有轨道解码完成
    duration_changed = Signal(int)  # 总时长变化 (ms)
    position_changed = Signal(int)  # 播放位置变化 (ms)
    eof_reached = Signal()  # 到达末尾
    error_occurred = Signal(int, str)  # (track_index, message)
    seek_completed = Signal(int)  # seek 完成 (position_ms)
    track_frame_decoded = Signal(int, int, bool)  # 单个轨道帧解码完成 (track_index, pts_ms, success)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._tracks: list[TrackState | None] = [None] * self.MAX_TRACKS
        self._track_count = 0
        self._current_position_ms = 0
        self._duration_ms = 0  # 所有轨道中最长的时长
        self._logger = get_logger()

        # Seek 状态跟踪
        self._pending_seek_count = 0  # 正在进行的 seek 数量
        self._last_seek_target_ms = 0

    # ========== 静态方法: 媒体探测 ==========

    @staticmethod
    def probe_file(path: str) -> MediaInfo:
        """探测媒体文件信息 (不需要创建解码器)"""
        native_info = voidview_native.probe_file(path)
        return MediaInfo.from_native(path, native_info)

    # ========== 公共 API ==========

    def add_track(self, index: int, path: str) -> bool:
        """添加轨道"""
        if not (0 <= index < self.MAX_TRACKS):
            return False
        if self._tracks[index] is not None:
            self.remove_track(index)

        # 探测媒体信息
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
            # 停止并销毁 DecodeWorker
            if track.worker:
                # 先清除回调，防止线程在销毁过程中调用已销毁的对象
                track.worker.set_callback(None)
                track.worker.stop()
                # DecodeWorker 析构函数会 join 线程，确保线程完全退出
                track.worker = None
            # 解码器会在 track 删除时自动清理
            track.decoder = None

        self._tracks[index] = None
        self._track_count = sum(1 for t in self._tracks if t is not None)
        self._update_duration()

    def initialize_decoder(self, index: int) -> bool:
        """
        初始化指定轨道的解码器

        注意: 必须在有 OpenGL 上下文的线程中调用
        """
        if not (0 <= index < self.MAX_TRACKS):
            return False

        track = self._tracks[index]
        if not track or not track.media_info:
            return False

        if track.decoder:
            return True  # 已初始化

        try:
            decoder = voidview_native.HardwareDecoder(track.path)
            if not decoder.initialize(0):  # Auto detect hardware type
                self.error_occurred.emit(index, decoder.error_message)
                return False

            # 设置 OpenGL 上下文 (0 表示使用当前上下文)
            decoder.set_opengl_context(0)

            track.decoder = decoder

            # 创建 DecodeWorker (C++ 后台线程)
            worker = voidview_native.DecodeWorker(decoder, index)
            worker.set_callback(self._on_decode_callback)
            track.worker = worker

            # 更新帧间隔
            if track.media_info.fps > 0:
                self._frame_interval_ms = int(1000 / track.media_info.fps)

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

    # ========== 播放控制 (由 PlaybackController 调用) ==========

    def request_frame(self, index: int):
        """
        请求指定轨道解码下一帧

        非阻塞：立即返回，解码完成后通过回调通知
        """
        track = self._tracks[index]
        if not track or not track.enabled or not track.worker or not track.decoder:
            return

        if track.decoder.eof:
            return

        track.worker.decode_frame()

    def request_all_frames(self):
        """请求所有活动轨道解码下一帧"""
        for track in self._tracks:
            if track and track.enabled and track.worker and track.decoder:
                if not track.decoder.eof:
                    track.worker.decode_frame()

    def cancel_all(self):
        """取消所有轨道的当前操作"""
        for track in self._tracks:
            if track and track.worker:
                track.worker.cancel()

    # ========== 帧缓冲 API (拉取模式) ==========

    def get_frame(self, index: int, timeout_ms: int = 50) -> tuple[bool, int]:
        """
        从指定轨道的帧队列取一帧

        Args:
            index: 轨道索引
            timeout_ms: 超时时间，-1 表示无限等待

        Returns:
            (success, pts_ms) - success=False 表示超时或 EOF
        """
        track = self._tracks[index]
        if not track or not track.worker or not track.decoder:
            return False, 0

        # 从队列取帧
        pts = track.worker.pop_frame(timeout_ms)
        if pts < 0:
            return False, 0

        return True, pts

    def get_frame_queue_size(self, index: int) -> int:
        """获取指定轨道的帧队列大小"""
        track = self._tracks[index]
        if not track or not track.worker:
            return 0
        return track.worker.frame_queue_size()

    def request_fill_buffers(self):
        """请求填充所有轨道的帧缓冲"""
        for track in self._tracks:
            if track and track.enabled and track.worker and track.decoder:
                if not track.decoder.eof:
                    track.worker.decode_frame()

    # ========== Seek 操作 ==========

    def seek_to(self, position_ms: int):
        """
        快速跳转到指定时间 (关键帧级别)

        非阻塞：提交命令到 C++ 后台线程立即返回
        """
        position_ms = max(0, min(position_ms, self._duration_ms))
        self._logger.info(f"[SEEK] DecoderPool.seek_to: {position_ms}ms (keyframe)")
        self._execute_seek(position_ms, is_precise=False)

    def seek_to_precise(self, position_ms: int):
        """
        精确跳转到指定时间之前的最近帧 (帧级别精确)

        非阻塞：提交命令到 C++ 后台线程立即返回
        """
        position_ms = max(0, min(position_ms, self._duration_ms))
        self._logger.info(f"[SEEK] DecoderPool.seek_to_precise: {position_ms}ms (frame-accurate)")
        self._execute_seek(position_ms, is_precise=True)

    def _execute_seek(self, target_ms: int, is_precise: bool):
        """执行 seek 操作 (非阻塞)"""
        t0 = time.perf_counter()

        # 取消之前的 seek
        for track in self._tracks:
            if track and track.worker:
                track.worker.cancel()

        # 跟踪正在进行的 seek
        self._pending_seek_count = 0
        self._last_seek_target_ms = target_ms

        # 向所有活动轨道提交 seek 命令
        for track in self._tracks:
            if not track or not track.decoder or not track.enabled or not track.worker:
                continue

            adjusted_ms = max(0, target_ms + track.offset_ms)
            self._pending_seek_count += 1

            if is_precise:
                track.worker.seek_precise(adjusted_ms)
            else:
                track.worker.seek_keyframe(adjusted_ms)

        self._logger.info(f"[SEEK] DecoderPool._execute_seek: submitted {self._pending_seek_count} seeks in {(time.perf_counter() - t0)*1000:.2f}ms")

    def _on_decode_callback(self, track_index: int, success: bool, pts_ms: int):
        """
        解码完成回调 (从 C++ 工作线程调用)

        通过 Qt 信号转发到主线程处理
        """
        try:
            # 使用 QMetaObject.invokeMethod 转发到主线程
            QMetaObject.invokeMethod(
                self,
                "_handle_decode_result",
                Qt.ConnectionType.QueuedConnection,
                Q_ARG(int, track_index),
                Q_ARG(bool, success),
                Q_ARG(int, pts_ms)
            )
        except RuntimeError:
            # 对象已被销毁 (窗口关闭时)，忽略回调
            pass

    @Slot(int, bool, int)
    def _handle_decode_result(self, track_index: int, success: bool, pts_ms: int):
        """在主线程处理解码结果"""
        track = self._tracks[track_index]
        if not track:
            return

        # 发送帧解码完成信号 (用于性能监控)
        self.track_frame_decoded.emit(track_index, pts_ms, success)

        # 新流程：帧在队列中，需要从队列取帧
        has_pending = track.decoder.has_pending_frame() if track.decoder else False
        queue_size = track.worker.frame_queue_size() if track.worker else 0

        self._logger.debug(f"_handle_decode_result: track={track_index}, success={success}, pts={pts_ms}, has_pending={has_pending}, queue_size={queue_size}")

        if not has_pending and track.worker and queue_size > 0:
            pop_success, pop_pts = self.get_frame(track_index, timeout_ms=0)
            if pop_success:
                has_pending = True
                pts_ms = pop_pts

        texture_uploaded = False
        if success or has_pending:
            track.current_pts_ms = pts_ms

            # 上传纹理 (必须在 GL 线程/主线程)
            if track.decoder and track.decoder.has_pending_frame():
                self._logger.debug(f"[SEEK] Uploading texture for track {track_index}, pts={pts_ms}")
                texture_uploaded = track.decoder.upload_pending_frame()
                self._logger.debug(f"[SEEK] Texture upload result: {texture_uploaded}")
                if not texture_uploaded:
                    self._logger.warning(f"Track {track_index}: texture upload failed")
        else:
            # 检查是否是 EOF 或错误
            if track.decoder:
                if track.decoder.eof:
                    self._logger.debug(f"Track {track_index} reached EOF")
                elif track.decoder.has_error:
                    self.error_occurred.emit(track_index, track.decoder.error_message)

        # 检查是否是 seek 操作完成
        if self._pending_seek_count > 0:
            self._pending_seek_count -= 1
            if self._pending_seek_count == 0:
                # 所有轨道 seek 完成
                self._on_all_seek_completed(self._last_seek_target_ms)
        else:
            # 播放过程中的帧解码完成
            self._check_all_tracks_ready()

    def _on_all_seek_completed(self, target_ms: int):
        """所有轨道 seek 完成"""
        self._logger.info(f"[SEEK] All seeks completed at {target_ms}ms")

        self._current_position_ms = target_ms
        self.position_changed.emit(target_ms)
        self.frame_ready.emit()
        self.seek_completed.emit(target_ms)

    def _check_all_tracks_ready(self):
        """检查所有轨道是否都完成了解码"""
        # 更新位置 (取所有轨道中的最大 PTS)
        max_pts = 0

        for track in self._tracks:
            if not track or not track.enabled or not track.decoder:
                continue

            if track.decoder.eof:
                continue

            pts = track.current_pts_ms - track.offset_ms
            max_pts = max(max_pts, pts)

        self._current_position_ms = max_pts
        self.position_changed.emit(self._current_position_ms)
        self.frame_ready.emit()

        # 检查是否所有活动轨道都到达 EOF
        all_eof = True
        has_active = False
        for track in self._tracks:
            if track and track.enabled and track.decoder:
                has_active = True
                if not track.decoder.eof:
                    all_eof = False
                    break

        if has_active and all_eof:
            self.eof_reached.emit()

    # ========== 单帧操作 ==========

    def step_frame(self):
        """单步进帧 (向前一帧)"""
        self.request_all_frames()

    def prev_frame(self):
        """上一帧 - 从 history 队列取帧

        使用双向帧队列，O(1) 直接取历史帧
        如果 history 为空则无法后退
        """
        # 尝试从所有轨道取历史帧
        all_success = True
        max_pts = 0

        for track in self._tracks:
            if not track or not track.enabled or not track.worker or not track.decoder:
                continue

            pts = track.worker.prev_frame()
            if pts >= 0:
                # 上传纹理
                if track.decoder.has_pending_frame():
                    track.decoder.upload_pending_frame()
                    track.current_pts_ms = pts
                max_pts = max(max_pts, pts - track.offset_ms)
            else:
                all_success = False

        if all_success:
            self._current_position_ms = max_pts
            self.position_changed.emit(self._current_position_ms)
            self.frame_ready.emit()

    def next_frame(self):
        """下一帧 - 从 future 队列取帧

        使用双向帧队列，O(1) 直接取未来帧
        触发填充 future 队列
        """
        all_success = True
        max_pts = 0

        for track in self._tracks:
            if not track or not track.enabled or not track.worker or not track.decoder:
                continue

            pts = track.worker.next_frame()
            if pts >= 0:
                # 上传纹理
                if track.decoder.has_pending_frame():
                    track.decoder.upload_pending_frame()
                    track.current_pts_ms = pts
                max_pts = max(max_pts, pts - track.offset_ms)
            else:
                # future 为空，触发解码
                track.worker.decode_frame()
                all_success = False

        if all_success:
            self._current_position_ms = max_pts
            self.position_changed.emit(self._current_position_ms)
            self.frame_ready.emit()


    # ========== 属性 ==========

    @property
    def current_position_ms(self) -> int:
        return self._current_position_ms

    # 别名，便于快捷键回调使用
    position_ms = current_position_ms

    @property
    def duration_ms(self) -> int:
        return self._duration_ms

    @property
    def track_count(self) -> int:
        return self._track_count

    # ========== 内部方法 ==========

    def _update_duration(self):
        """更新总时长 (所有轨道中最长的)"""
        max_duration = 0
        for track in self._tracks:
            if track and track.media_info:
                # 考虑偏移后的实际时长
                duration = track.media_info.duration_ms - track.offset_ms
                max_duration = max(max_duration, duration)

        if max_duration != self._duration_ms:
            self._duration_ms = max_duration
            self.duration_changed.emit(self._duration_ms)

    def clear(self):
        """清除所有轨道"""
        # 停止所有 DecodeWorker
        for track in self._tracks:
            if track and track.worker:
                # 先清除回调，防止线程在销毁过程中调用已销毁的对象
                track.worker.set_callback(None)
                track.worker.stop()

        self._tracks = [None] * self.MAX_TRACKS
        self._track_count = 0
        self._current_position_ms = 0
        self._duration_ms = 0
        self.duration_changed.emit(0)
