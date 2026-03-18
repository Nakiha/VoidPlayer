"""
DecoderPool - 多轨道解码器管理器

负责:
- 管理多个 HardwareDecoder 实例
- 提供媒体信息探测 (probe_file)
- 同步多轨道解码
- 播放控制 (播放/暂停/seek)
"""
from dataclasses import dataclass
from typing import TYPE_CHECKING

from PySide6.QtCore import QObject, Signal, QTimer

from player.native import voidview_native

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

    信号:
        frame_ready: 所有活动轨道解码完一帧
        duration_changed: 总时长变化
        position_changed: 播放位置变化 (ms)
        eof_reached: 到达文件末尾
        error_occurred: 发生错误 (index, message)
    """

    MAX_TRACKS = 8

    # 信号
    frame_ready = Signal()  # 所有轨道解码完成
    duration_changed = Signal(int)  # 总时长变化 (ms)
    position_changed = Signal(int)  # 播放位置变化 (ms)
    eof_reached = Signal()  # 到达末尾
    error_occurred = Signal(int, str)  # (track_index, message)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._tracks: list[TrackState | None] = [None] * self.MAX_TRACKS
        self._track_count = 0
        self._is_playing = False
        self._current_position_ms = 0
        self._duration_ms = 0  # 所有轨道中最长的时长

        # 播放定时器
        self._play_timer = QTimer(self)
        self._play_timer.timeout.connect(self._decode_frame)
        self._frame_interval_ms = 16  # ~60fps，会根据视频帧率调整

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
        if track and track.decoder:
            # 解码器会在 track 删除时自动清理
            pass

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

    # ========== 播放控制 ==========

    def play(self):
        """开始播放"""
        if self._is_playing:
            return

        # 确保所有活动轨道的解码器已初始化
        for track in self._tracks:
            if track and track.enabled and not track.decoder:
                # 延迟初始化 - 由 GLWidget 在 initializeGL 中调用
                pass

        self._is_playing = True
        self._play_timer.start(self._frame_interval_ms)

    def pause(self):
        """暂停播放"""
        self._is_playing = False
        self._play_timer.stop()

    def toggle_play(self):
        """切换播放/暂停"""
        if self._is_playing:
            self.pause()
        else:
            self.play()

    def seek_to(self, position_ms: int):
        """跳转到指定时间"""
        position_ms = max(0, min(position_ms, self._duration_ms))
        self._current_position_ms = position_ms

        for track in self._tracks:
            if track and track.decoder and track.enabled:
                target_ms = position_ms + track.offset_ms
                target_ms = max(0, target_ms)
                track.decoder.seek_to(target_ms)

        # seek 后立即解码一帧以更新纹理
        # _decode_frame 会发出 position_changed 和 frame_ready 信号
        self._decode_frame()

    def step_frame(self):
        """单步进帧"""
        self._decode_frame()

    def prev_frame(self):
        """上一帧 - 向后 seek 一帧时间"""
        if self._frame_interval_ms > 0:
            new_pos = max(0, self._current_position_ms - self._frame_interval_ms)
            self.seek_to(new_pos)

    def next_frame(self):
        """下一帧 - 前进一帧"""
        if self._frame_interval_ms > 0:
            new_pos = min(self._duration_ms, self._current_position_ms + self._frame_interval_ms)
            self.seek_to(new_pos)

    # ========== 属性 ==========

    @property
    def is_playing(self) -> bool:
        return self._is_playing

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

    def _decode_frame(self):
        """解码一帧 (所有活动轨道)"""
        any_decoded = False
        has_active_track = False

        for track in self._tracks:
            if not track or not track.enabled:
                continue
            if not track.decoder:
                continue

            has_active_track = True

            if track.decoder.eof:
                continue

            if track.decoder.decode_next_frame():
                track.current_pts_ms = track.decoder.current_pts_ms
                any_decoded = True
            elif track.decoder.eof:
                pass  # 到达 EOF
            elif track.decoder.has_error:
                self.error_occurred.emit(track.index, track.decoder.error_message)

        if any_decoded:
            # 更新位置 (取所有轨道中的最大 PTS)
            max_pts = 0
            for track in self._tracks:
                if track and track.decoder and not track.decoder.eof:
                    pts = track.current_pts_ms - track.offset_ms
                    max_pts = max(max_pts, pts)
            self._current_position_ms = max_pts
            self.position_changed.emit(self._current_position_ms)
            self.frame_ready.emit()

        # 检查是否所有活动轨道都到达 EOF
        if has_active_track:
            all_eof = True
            for track in self._tracks:
                if track and track.enabled and track.decoder:
                    if not track.decoder.eof:
                        all_eof = False
                        break
            if all_eof:
                self.pause()
                self.eof_reached.emit()

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
        self.pause()
        self._tracks = [None] * self.MAX_TRACKS
        self._track_count = 0
        self._current_position_ms = 0
        self._duration_ms = 0
        self.duration_changed.emit(0)
