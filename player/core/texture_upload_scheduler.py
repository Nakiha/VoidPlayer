"""
TextureUploadScheduler - 纹理上传调度器

协调从解码线程到 GL 线程的纹理上传。
确保所有 OpenGL 操作都在正确的上下文中执行。
"""
from __future__ import annotations

from typing import TYPE_CHECKING, Set
from PySide6.QtCore import QObject, Signal, Qt, QMetaObject, Q_ARG

if TYPE_CHECKING:
    from player.core.decode_worker import DecodeWorker
    from player.ui.viewport.gl_widget import MultiTrackGLWidget

from player.core.logging_config import get_logger


class TextureUploadScheduler(QObject):
    """
    纹理上传调度器

    负责在正确的 OpenGL 上下文中调度纹理上传操作。
    解码线程完成解码后，通知此调度器，调度器确保上传在 GL 线程执行。

    使用方式:
    1. 在 GLWidget 初始化时设置此调度器
    2. 解码完成后调用 schedule_upload()
    3. GLWidget 在 paintGL 前调用 process_pending_uploads()

    信号:
        uploads_pending: () 有待上传的帧
        upload_completed: (track_index) 上传完成
    """

    uploads_pending = Signal()  # 有待上传的帧
    upload_completed = Signal(int)  # (track_index)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._logger = get_logger()

        # 待上传的轨道索引集合
        self._pending_uploads: Set[int] = set()

        # Worker 引用 (弱引用，由外部管理生命周期)
        self._workers: list[DecodeWorker | None] = [None] * 8  # MAX_TRACKS

        # GL Widget 引用
        self._gl_widget: MultiTrackGLWidget | None = None

    def set_worker(self, track_index: int, worker: DecodeWorker | None):
        """设置轨道对应的 worker"""
        if 0 <= track_index < len(self._workers):
            self._workers[track_index] = worker

    def remove_worker(self, track_index: int):
        """移除轨道对应的 worker"""
        if 0 <= track_index < len(self._workers):
            self._workers[track_index] = None

    def set_gl_widget(self, widget: MultiTrackGLWidget | None):
        """设置 GL Widget"""
        self._gl_widget = widget

    def schedule_upload(self, track_index: int):
        """
        调度纹理上传 (可在任意线程调用)

        Args:
            track_index: 轨道索引
        """
        if not (0 <= track_index < len(self._workers)):
            return

        self._pending_uploads.add(track_index)
        self.uploads_pending.emit()

        self._logger.trace(f"TextureUploadScheduler: scheduled upload for track {track_index}")

    def process_pending_uploads(self) -> int:
        """
        处理所有待上传帧 (必须在 GL 上下文调用)

        Returns:
            成功上传的帧数
        """
        uploaded_count = 0

        # 复制集合避免迭代时修改
        pending = list(self._pending_uploads)

        for track_index in pending:
            worker = self._workers[track_index] if track_index < len(self._workers) else None

            if worker is None:
                self._pending_uploads.discard(track_index)
                continue

            if not worker.has_pending_frame():
                self._pending_uploads.discard(track_index)
                continue

            try:
                success = worker.upload_pending_frame()
                if success:
                    uploaded_count += 1
                    self._pending_uploads.discard(track_index)
                    self.upload_completed.emit(track_index)
                    self._logger.trace(f"TextureUploadScheduler: uploaded frame for track {track_index}")
                else:
                    self._logger.warning(f"TextureUploadScheduler: failed to upload frame for track {track_index}")

            except Exception as e:
                self._logger.error(f"TextureUploadScheduler: upload error for track {track_index} - {e}")
                self._pending_uploads.discard(track_index)

        return uploaded_count

    def has_pending_uploads(self) -> bool:
        """检查是否有待上传的帧"""
        return len(self._pending_uploads) > 0

    def clear_pending(self):
        """清除所有待上传任务"""
        self._pending_uploads.clear()

    def get_pending_tracks(self) -> Set[int]:
        """获取待上传的轨道索引集合"""
        return self._pending_uploads.copy()


class GLSyncHelper:
    """
    GL 同步助手

    用于在非 GL 线程请求 GL 操作。
    配合 MultiTrackGLWidget 使用。
    """

    def __init__(self, gl_widget: MultiTrackGLWidget):
        self._gl_widget = gl_widget
        self._logger = get_logger()

    def request_upload(self, scheduler: TextureUploadScheduler):
        """
        请求 GL Widget 处理待上传帧

        通过 Qt 的 invokeMethod 确保 upload 在 GL 线程执行。
        """
        if self._gl_widget is None:
            return

        # 使用 Qt 的元对象系统在 GL Widget 的线程调用
        QMetaObject.invokeMethod(
            self._gl_widget,
            "processPendingUploads",
            Qt.ConnectionType.QueuedConnection
        )
