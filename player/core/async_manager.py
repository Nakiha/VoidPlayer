"""
AsyncOperationManager - 异步操作管理器

管理所有异步 native 操作的生命周期，提供取消支持。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Callable, Generic, TypeVar, Any
from concurrent.futures import Future, ThreadPoolExecutor
from PySide6.QtCore import QObject, Signal, QMetaObject, Qt, Q_ARG

from player.core.logging_config import get_logger

T = TypeVar('T')


class OperationState(Enum):
    """操作状态"""
    PENDING = auto()      # 等待执行
    RUNNING = auto()      # 正在执行
    COMPLETED = auto()    # 已完成
    CANCELLED = auto()    # 已取消
    FAILED = auto()       # 失败


@dataclass
class AsyncResult(Generic[T]):
    """异步操作结果"""
    operation_id: int
    state: OperationState
    result: T | None = None
    error: str | None = None


class AsyncOperationManager(QObject):
    """
    管理所有异步 native 操作的生命周期

    功能:
    - 提交异步任务到线程池
    - 跟踪操作状态
    - 支持取消操作
    - 通过信号通知结果

    信号:
        operation_completed: (operation_id, result) 操作完成
        operation_failed: (operation_id, error_message) 操作失败
        operation_cancelled: (operation_id) 操作被取消
    """

    operation_completed = Signal(int, object)  # (operation_id, result)
    operation_failed = Signal(int, str)  # (operation_id, error_message)
    operation_cancelled = Signal(int)  # (operation_id)

    def __init__(self, parent=None, max_workers: int = 4):
        super().__init__(parent)
        self._logger = get_logger()
        self._executor = ThreadPoolExecutor(max_workers=max_workers)
        self._operations: dict[int, Future] = {}
        self._cancel_tokens: dict[int, Any] = {}  # CancelToken 对象
        self._callbacks: dict[int, Callable] = {}
        self._next_id = 0
        self._lock = None  # 延迟初始化，避免在主线程外使用 QMutex

    @property
    def _thread_lock(self):
        """延迟初始化线程锁"""
        if self._lock is None:
            import threading
            self._lock = threading.Lock()
        return self._lock

    def submit(
        self,
        func: Callable[..., T],
        *args,
        cancel_token: Any = None,
        callback: Callable[[AsyncResult[T]], None] | None = None,
        **kwargs
    ) -> int:
        """
        提交异步任务

        Args:
            func: 要执行的函数
            *args: 函数参数
            cancel_token: 取消令牌 (voidview_native.CancelToken)
            callback: 完成回调 (可选)
            **kwargs: 函数关键字参数

        Returns:
            operation_id: 操作 ID，用于取消或查询状态
        """
        with self._thread_lock:
            op_id = self._next_id
            self._next_id += 1

            if cancel_token is not None:
                self._cancel_tokens[op_id] = cancel_token

            if callback is not None:
                self._callbacks[op_id] = callback

        future = self._executor.submit(self._run_operation, op_id, func, *args, **kwargs)
        future.add_done_callback(lambda f: self._on_operation_done(op_id, f))

        with self._thread_lock:
            self._operations[op_id] = future

        self._logger.debug(f"AsyncOperation[{op_id}]: submitted")
        return op_id

    def _run_operation(self, op_id: int, func: Callable[..., T], *args, **kwargs) -> AsyncResult[T]:
        """执行操作 (在工作线程中运行)"""
        result = AsyncResult(operation_id=op_id, state=OperationState.RUNNING)

        try:
            ret = func(*args, **kwargs)
            result.state = OperationState.COMPLETED
            result.result = ret
        except Exception as e:
            result.state = OperationState.FAILED
            result.error = str(e)
            self._logger.error(f"AsyncOperation[{op_id}]: failed - {e}")

        return result

    def _on_operation_done(self, op_id: int, future: Future):
        """操作完成回调"""
        with self._thread_lock:
            self._operations.pop(op_id, None)
            cancel_token = self._cancel_tokens.pop(op_id, None)
            callback = self._callbacks.pop(op_id, None)

        try:
            result = future.result()

            # 检查是否被取消
            if cancel_token is not None and cancel_token.is_cancelled():
                result.state = OperationState.CANCELLED

            # 根据状态发射信号
            if result.state == OperationState.COMPLETED:
                self.operation_completed.emit(op_id, result.result)
            elif result.state == OperationState.CANCELLED:
                self.operation_cancelled.emit(op_id)
            elif result.state == OperationState.FAILED:
                self.operation_failed.emit(op_id, result.error or "Unknown error")

            # 调用回调
            if callback is not None:
                callback(result)

            self._logger.debug(f"AsyncOperation[{op_id}]: {result.state.name}")

        except Exception as e:
            self._logger.error(f"AsyncOperation[{op_id}]: exception in done callback - {e}")
            self.operation_failed.emit(op_id, str(e))

    def cancel(self, operation_id: int) -> bool:
        """
        请求取消操作

        Args:
            operation_id: 操作 ID

        Returns:
            True 如果成功请求取消，False 如果操作不存在或已完成
        """
        with self._thread_lock:
            cancel_token = self._cancel_tokens.get(operation_id)
            future = self._operations.get(operation_id)

        if cancel_token is not None and future is not None and not future.done():
            cancel_token.cancel()
            self._logger.debug(f"AsyncOperation[{operation_id}]: cancellation requested")
            return True

        return False

    def cancel_all(self):
        """取消所有正在进行的操作"""
        with self._thread_lock:
            for op_id, cancel_token in self._cancel_tokens.items():
                if cancel_token is not None:
                    cancel_token.cancel()

        self._logger.debug("All operations cancellation requested")

    def get_state(self, operation_id: int) -> OperationState | None:
        """获取操作状态"""
        with self._thread_lock:
            future = self._operations.get(operation_id)
            cancel_token = self._cancel_tokens.get(operation_id)

        # 如果 future 不存在，说明操作已完成并从字典中移除
        if future is None:
            # 检查取消令牌是否还存在 (如果存在说明被取消了)
            if cancel_token is not None and cancel_token.is_cancelled():
                return OperationState.CANCELLED
            # 否则假设已完成
            return None  # 已经完成并清理

        if cancel_token is not None and cancel_token.is_cancelled():
            return OperationState.CANCELLED

        if future.running():
            return OperationState.RUNNING

        if future.done():
            try:
                future.result()
                return OperationState.COMPLETED
            except Exception:
                return OperationState.FAILED

        return OperationState.PENDING

    def shutdown(self, wait: bool = True):
        """关闭管理器"""
        self.cancel_all()
        self._executor.shutdown(wait=wait)
        self._logger.debug("AsyncOperationManager shutdown")
