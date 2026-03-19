"""
DecodeWorker - 独立解码线程

每个轨道一个 DecodeWorker，负责在后台线程中执行解码操作。
支持取消机制，适用于精确 seek 等长时间操作。
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
from typing import TYPE_CHECKING, Callable
from PySide6.QtCore import QObject, Signal, QThread, QMutex, QMutexLocker, QWaitCondition

if TYPE_CHECKING:
    from player.native import voidview_native

from player.core.logging_config import get_logger


class CommandType(Enum):
    """解码命令类型"""
    DECODE = auto()        # 解码下一帧
    SEEK = auto()          # 快速 seek (关键帧级别)
    SEEK_PRECISE = auto()  # 精确 seek (可取消)
    STOP = auto()          # 停止线程


@dataclass
class DecodeCommand:
    """解码命令"""
    command: CommandType
    timestamp_ms: int | None = None
    callback: Callable[[bool, int | None], None] | None = None  # (success, pts_ms)


class DecodeWorker(QObject):
    """
    独立解码线程 (每个轨道一个)

    在后台线程中执行解码操作，避免阻塞 UI 线程。
    支持:
    - 异步解码
    - 可取消的精确 seek
    - 命令队列

    信号:
        frame_decoded: (track_index) 有新帧待上传
        seek_completed: (track_index, position_ms) seek 完成
        seek_cancelled: (track_index) seek 被取消
        error_occurred: (track_index, message) 发生错误
        frame_upload_ready: (track_index) 帧已解码，请求 GL 线程上传
    """

    frame_decoded = Signal(int)  # (track_index)
    seek_completed = Signal(int, int)  # (track_index, position_ms)
    seek_cancelled = Signal(int)  # (track_index)
    error_occurred = Signal(int, str)  # (track_index, message)
    frame_upload_ready = Signal(int)  # (track_index) 请求 GL 线程上传

    def __init__(self, track_index: int, parent=None):
        super().__init__(parent)
        self._logger = get_logger()
        self._track_index = track_index

        # 解码器和取消令牌
        self._decoder: voidview_native.HardwareDecoder | None = None
        self._cancel_token: voidview_native.CancelToken | None = None

        # 命令队列
        self._command_queue: list[DecodeCommand] = []
        self._mutex = QMutex()
        self._wait_condition = QWaitCondition()
        self._running = False

        # 当前操作
        self._current_command: DecodeCommand | None = None

        # 线程
        self._thread = QThread()
        self.moveToThread(self._thread)
        self._thread.started.connect(self._run)

    @property
    def track_index(self) -> int:
        return self._track_index

    @property
    def decoder(self) -> voidview_native.HardwareDecoder | None:
        return self._decoder

    def set_decoder(self, decoder: voidview_native.HardwareDecoder):
        """设置解码器"""
        self._decoder = decoder
        # 创建新的取消令牌
        from player.native import voidview_native
        self._cancel_token = voidview_native.CancelToken()

    def start(self):
        """启动解码线程"""
        if not self._running:
            self._running = True
            self._thread.start()
            self._logger.debug(f"DecodeWorker[{self._track_index}]: started")

    def stop(self):
        """停止解码线程"""
        self._running = False
        self.cancel_current()
        self._wait_condition.wakeAll()
        self._thread.quit()
        self._thread.wait(2000)  # 等待最多 2 秒
        self._logger.debug(f"DecodeWorker[{self._track_index}]: stopped")

    def post_command(self, cmd: DecodeCommand):
        """
        投递命令 (线程安全)

        Args:
            cmd: 解码命令
        """
        with QMutexLocker(self._mutex):
            # 如果是新的 seek 命令，清除之前的 seek 命令
            if cmd.command in (CommandType.SEEK, CommandType.SEEK_PRECISE):
                self._command_queue = [
                    c for c in self._command_queue
                    if c.command not in (CommandType.SEEK, CommandType.SEEK_PRECISE)
                ]

            self._command_queue.append(cmd)
            self._wait_condition.wakeOne()

        self._logger.trace(f"DecodeWorker[{self._track_index}]: posted {cmd.command.name}")

    def cancel_current(self):
        """取消当前操作"""
        if self._cancel_token is not None:
            self._cancel_token.cancel()
            self._logger.debug(f"DecodeWorker[{self._track_index}]: cancellation requested")

    def has_pending_frame(self) -> bool:
        """检查是否有待上传的帧"""
        if self._decoder is not None:
            return self._decoder.has_pending_frame()
        return False

    def upload_pending_frame(self) -> bool:
        """
        上传待上传帧 (必须在 GL 线程调用)

        Returns:
            True 如果上传成功
        """
        if self._decoder is not None and self._decoder.has_pending_frame():
            return self._decoder.upload_pending_frame()
        return False

    def get_texture_id(self) -> int:
        """获取当前纹理 ID"""
        if self._decoder is not None:
            return self._decoder.texture_id
        return 0

    def get_current_pts_ms(self) -> int:
        """获取当前 PTS"""
        if self._decoder is not None:
            return self._decoder.current_pts_ms
        return 0

    def _run(self):
        """解码循环 (在工作线程中运行)"""
        self._logger.debug(f"DecodeWorker[{self._track_index}]: run loop started")

        while self._running:
            # 获取下一个命令
            cmd = self._get_next_command()

            if cmd is None or cmd.command == CommandType.STOP:
                break

            self._current_command = cmd
            self._execute_command(cmd)
            self._current_command = None

        self._logger.debug(f"DecodeWorker[{self._track_index}]: run loop ended")

    def _get_next_command(self) -> DecodeCommand | None:
        """获取下一个命令 (阻塞等待)"""
        with QMutexLocker(self._mutex):
            while not self._command_queue and self._running:
                self._wait_condition.wait(self._mutex)

            if self._command_queue:
                return self._command_queue.pop(0)

        return None

    def _execute_command(self, cmd: DecodeCommand):
        """执行命令"""
        if self._decoder is None:
            self._logger.warning(f"DecodeWorker[{self._track_index}]: no decoder set")
            if cmd.callback:
                cmd.callback(False, None)
            return

        if cmd.command == CommandType.DECODE:
            self._execute_decode(cmd)
        elif cmd.command == CommandType.SEEK:
            self._execute_seek(cmd)
        elif cmd.command == CommandType.SEEK_PRECISE:
            self._execute_seek_precise(cmd)

    def _execute_decode(self, cmd: DecodeCommand):
        """执行解码命令"""
        try:
            # 重置取消令牌
            if self._cancel_token:
                self._cancel_token.reset()

            success = self._decoder.decode_next_frame_async(self._cancel_token)

            if success:
                # 通知有新帧待上传
                self.frame_upload_ready.emit(self._track_index)
                self.frame_decoded.emit(self._track_index)

                if cmd.callback:
                    cmd.callback(True, self._decoder.current_pts_ms)
            else:
                # 检查是否被取消
                if self._cancel_token and self._cancel_token.is_cancelled():
                    self._logger.debug(f"DecodeWorker[{self._track_index}]: decode cancelled")
                elif self._decoder.has_error:
                    self.error_occurred.emit(self._track_index, self._decoder.error_message)

                if cmd.callback:
                    cmd.callback(False, None)

        except Exception as e:
            self._logger.error(f"DecodeWorker[{self._track_index}]: decode error - {e}")
            self.error_occurred.emit(self._track_index, str(e))
            if cmd.callback:
                cmd.callback(False, None)

    def _execute_seek(self, cmd: DecodeCommand):
        """执行快速 seek 命令 (关键帧级别)"""
        if cmd.timestamp_ms is None:
            return

        try:
            success = self._decoder.seek_to(cmd.timestamp_ms)

            if success:
                # Seek 后解码一帧
                if self._cancel_token:
                    self._cancel_token.reset()
                decode_success = self._decoder.decode_next_frame_async(self._cancel_token)

                if decode_success:
                    self.frame_upload_ready.emit(self._track_index)
                    self.seek_completed.emit(self._track_index, self._decoder.current_pts_ms)
                else:
                    self.seek_completed.emit(self._track_index, cmd.timestamp_ms)
            else:
                if self._decoder.has_error:
                    self.error_occurred.emit(self._track_index, self._decoder.error_message)

            if cmd.callback:
                cmd.callback(success, self._decoder.current_pts_ms if success else None)

        except Exception as e:
            self._logger.error(f"DecodeWorker[{self._track_index}]: seek error - {e}")
            self.error_occurred.emit(self._track_index, str(e))
            if cmd.callback:
                cmd.callback(False, None)

    def _execute_seek_precise(self, cmd: DecodeCommand):
        """执行精确 seek 命令 (可取消)"""
        if cmd.timestamp_ms is None:
            return

        try:
            # 重置取消令牌
            if self._cancel_token:
                self._cancel_token.reset()

            success = self._decoder.seek_to_precise_async(
                cmd.timestamp_ms,
                self._cancel_token
            )

            if success:
                # 通知有新帧待上传
                self.frame_upload_ready.emit(self._track_index)
                self.seek_completed.emit(self._track_index, self._decoder.current_pts_ms)

                if cmd.callback:
                    cmd.callback(True, self._decoder.current_pts_ms)
            else:
                # 检查是否被取消
                if self._cancel_token and self._cancel_token.is_cancelled():
                    self._logger.debug(f"DecodeWorker[{self._track_index}]: seek cancelled at {cmd.timestamp_ms}ms")
                    self.seek_cancelled.emit(self._track_index)
                elif self._decoder.has_error:
                    self.error_occurred.emit(self._track_index, self._decoder.error_message)

                if cmd.callback:
                    cmd.callback(False, None)

        except Exception as e:
            self._logger.error(f"DecodeWorker[{self._track_index}]: seek_precise error - {e}")
            self.error_occurred.emit(self._track_index, str(e))
            if cmd.callback:
                cmd.callback(False, None)


class DecodeWorkerPool:
    """
    解码线程池

    管理多个 DecodeWorker 实例。
    """

    MAX_TRACKS = 8

    def __init__(self):
        self._workers: list[DecodeWorker | None] = [None] * self.MAX_TRACKS
        self._logger = get_logger()

    def get_worker(self, track_index: int) -> DecodeWorker | None:
        """获取指定轨道的 worker"""
        if 0 <= track_index < self.MAX_TRACKS:
            return self._workers[track_index]
        return None

    def create_worker(self, track_index: int) -> DecodeWorker:
        """创建并启动 worker"""
        if not (0 <= track_index < self.MAX_TRACKS):
            raise ValueError(f"Invalid track index: {track_index}")

        # 停止并移除现有 worker
        self.remove_worker(track_index)

        worker = DecodeWorker(track_index)
        self._workers[track_index] = worker
        worker.start()

        return worker

    def remove_worker(self, track_index: int):
        """移除 worker"""
        if 0 <= track_index < self.MAX_TRACKS:
            worker = self._workers[track_index]
            if worker is not None:
                worker.stop()
                worker.deleteLater()
            self._workers[track_index] = None

    def cancel_all(self):
        """取消所有 worker 的当前操作"""
        for worker in self._workers:
            if worker is not None:
                worker.cancel_current()

    def stop_all(self):
        """停止所有 worker"""
        for i in range(self.MAX_TRACKS):
            self.remove_worker(i)

    def broadcast_seek(self, timestamp_ms: int, precise: bool = False):
        """向所有 worker 广播 seek 命令"""
        cmd_type = CommandType.SEEK_PRECISE if precise else CommandType.SEEK
        for worker in self._workers:
            if worker is not None and worker.decoder is not None:
                worker.post_command(DecodeCommand(cmd_type, timestamp_ms))

    def cancel_all_seeks(self):
        """取消所有正在进行的 seek"""
        for worker in self._workers:
            if worker is not None:
                worker.cancel_current()
