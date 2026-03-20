# 异步 Native 调用架构设计

## 问题分析

### 当前阻塞调用

| 操作 | 阻塞级别 | 原因 | 可取消性 |
|------|---------|------|---------|
| `probe_file()` | 高 | `avformat_find_stream_info()` 需要读取多帧 | 无意义 (必须完成才能返回) |
| `HardwareDecoder.__init__()` | 高 | 同上 | 无意义 |
| `initialize()` | 中 | `avcodec_open2()`, 硬件设备初始化 | 无意义 |
| `set_opengl_context()` | 低-中 | WGL 扩展加载, D3D11 设备共享 | 无意义 |
| `decode_next_frame()` | 中 | `av_read_frame()` + `avcodec_receive_frame()` | 有意义 (可打断长时间解码) |
| `seek_to()` | 中 | `av_seek_frame()` | 无意义 (单次 I/O 操作) |
| `seek_to_precise()` | **高** | 逐帧解码到目标位置 | **有意义** (可打断后续解码) |
| 软解帧上传 | 高 | CPU YUV→RGBA + `glTexSubImage2D()` | 有意义 |

### 核心问题

1. **UI 线程阻塞**: 所有 native 调用都在主线程执行，导致 UI 无响应
2. **无法取消 seek**: 精确 seek 需要逐帧解码，用户快速拖动时无法打断
3. **OpenGL 上下文约束**: 硬解 ZeroCopy 和软解帧上传都需要在 GL 上下文中执行

## 架构设计

### 设计原则

1. **分离关注点**: I/O 密集操作与 GL 依赖操作分离
2. **可取消性**: 长时间操作支持异步取消
3. **上下文隔离**: OpenGL 操作统一管理，确保线程安全
4. **向后兼容**: 保留同步 API，新增异步 API

### 整体架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Python Layer (UI Thread)                   │
├─────────────────────────────────────────────────────────────────────┤
│  DecoderPoolAsync                                                    │
│  ├── AsyncOperationManager (管理异步任务生命周期)                      │
│  └── Signal/Slot 通信 (UI 更新)                                      │
└───────────────────────────┬─────────────────────────────────────────┘
                            │ QThreadPool / QRunnable
                            ▼
┌─────────────────────────────────────────────────────────────────────┐
│                       Worker Thread Pool                             │
├─────────────────────────────────────────────────────────────────────┤
│  I/O Worker (无 GL 依赖)                                             │
│  ├── probe_file_async()                                              │
│  ├── open_source_async()  → avformat_open_input + find_stream_info  │
│  └── init_decoder_async() → avcodec_open2 + hw_device_ctx_create    │
└───────────────────────────┬─────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    Decode Thread (Per-Track)                         │
├─────────────────────────────────────────────────────────────────────┤
│  DecodeWorker                                                        │
│  ├── decode_loop()        → 持续解码到 FrameQueue                     │
│  ├── seek_async()         → seek_to (关键帧级别)                      │
│  ├── seek_precise_async() → seek_to_precise (可取消)                 │
│  └── cancel_token         → std::atomic<bool> 取消标志               │
└───────────────────────────┬─────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      GL Thread (UI Thread)                           │
├─────────────────────────────────────────────────────────────────────┤
│  TextureUploadManager                                                │
│  ├── upload_frame()        → 从 FrameQueue 取帧, 上传纹理            │
│  │   ├── 硬解: bind_frame() (WGL_NV_DX_interop)                     │
│  │   └── 软解: glTexSubImage2D()                                    │
│  └── 必须在 QOpenGLWidget.initializeGL/paintGL 上下文执行            │
└─────────────────────────────────────────────────────────────────────┘
```

### Native 层改动

#### 1. 新增取消令牌 (CancelToken)

```cpp
// native/include/voidview_native/cancel_token.hpp
namespace voidview {
class CancelToken {
public:
    CancelToken() = default;
    void cancel() { cancelled_.store(true); }
    bool is_cancelled() const { return cancelled_.load(); }
    void reset() { cancelled_.store(false); }
private:
    std::atomic<bool> cancelled_{false};
};
}
```

#### 2. 新增异步解码接口

```cpp
// native/include/voidview_native/hardware_decoder.hpp (扩展)

class HardwareDecoder {
public:
    // 现有同步接口保持不变...

    // === 新增异步/可取消接口 ===

    /**
     * 解码下一帧 (可取消)
     * @param token 取消令牌
     * @return True if new frame decoded
     */
    bool decode_next_frame(CancelToken& token);

    /**
     * 精确 seek (可取消)
     * 在每个解码循环中检查 token.is_cancelled()
     * @param timestamp_ms 目标时间
     * @param token 取消令牌
     * @return True if completed, False if cancelled
     */
    bool seek_to_precise(int64_t timestamp_ms, CancelToken& token);

    /**
     * 检查是否有已解码帧等待上传
     */
    bool has_pending_frame() const;

    /**
     * 获取已解码帧 (不上传纹理)
     * 用于异步解码后由 GL 线程上传
     */
    AVFrame* get_pending_frame();

    /**
     * 上传已解码帧到纹理 (必须在 GL 上下文调用)
     */
    bool upload_pending_frame();
};
```

#### 3. 修改 seek_frame_precise 实现

```cpp
// native/src/hardware_decoder.cpp

bool HardwareDecoder::Impl::seek_frame_precise(int64_t timestamp_ms, CancelToken* token) {
    // ... seek to keyframe ...

    while (true) {
        // 检查取消
        if (token && token->is_cancelled()) {
            return false;  // 被取消
        }

        ret = av_read_frame(fmt_ctx_, pkt_);
        // ... 解码循环 ...

        // 每解码一帧检查取消
        if (token && token->is_cancelled()) {
            return false;
        }
    }
}
```

### Python 层架构

#### 1. AsyncOperationManager

```python
# player/core/async_manager.py

from dataclasses import dataclass
from enum import Enum, auto
from typing import Generic, TypeVar
from concurrent.futures import Future
from PySide6.QtCore import QObject, Signal, QThreadPool, QRunnable

T = TypeVar('T')

class OperationState(Enum):
    PENDING = auto()
    RUNNING = auto()
    COMPLETED = auto()
    CANCELLED = auto()
    FAILED = auto()

@dataclass
class AsyncResult(Generic[T]):
    operation_id: int
    state: OperationState
    result: T | None = None
    error: str | None = None

class AsyncOperationManager(QObject):
    """管理所有异步 native 操作的生命周期"""

    operation_completed = Signal(int, object)  # (operation_id, result)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._thread_pool = QThreadPool.globalInstance()
        self._operations: dict[int, Future] = {}
        self._cancel_tokens: dict[int, 'CancelToken'] = {}
        self._next_id = 0

    def submit(self, func, *args, **kwargs) -> int:
        """提交异步任务，返回操作 ID"""
        op_id = self._next_id
        self._next_id += 1

        # ... 提交到线程池 ...
        return op_id

    def cancel(self, operation_id: int) -> bool:
        """取消操作"""
        if operation_id in self._cancel_tokens:
            self._cancel_tokens[operation_id].cancel()
            return True
        return False
```

#### 2. DecodeWorker (解码线程)

```python
# player/core/decode_worker.py

from dataclasses import dataclass
from typing import TYPE_CHECKING
from PySide6.QtCore import QObject, Signal, QThread, QMutex, QWaitCondition

if TYPE_CHECKING:
    from player.native import voidview_native

@dataclass
class DecodeCommand:
    """解码命令"""
    command: str  # 'decode', 'seek', 'seek_precise', 'stop'
    timestamp_ms: int | None = None

class DecodeWorker(QObject):
    """独立解码线程 (每个轨道一个)"""

    frame_decoded = Signal(int)  # (track_index) 通知有新帧待上传
    seek_completed = Signal(int, int)  # (track_index, position_ms)
    error_occurred = Signal(int, str)  # (track_index, message)

    def __init__(self, track_index: int, parent=None):
        super().__init__(parent)
        self._track_index = track_index
        self._decoder: "voidview_native.HardwareDecoder | None" = None
        self._cancel_token = voidview_native.CancelToken()

        self._command_queue: list[DecodeCommand] = []
        self._mutex = QMutex()
        self._wait_condition = QWaitCondition()
        self._running = False

    def post_command(self, cmd: DecodeCommand):
        """投递命令 (线程安全)"""
        with QMutexLocker(self._mutex):
            self._command_queue.append(cmd)
            self._wait_condition.wakeOne()

    def cancel_current(self):
        """取消当前操作"""
        self._cancel_token.cancel()

    def run(self):
        """解码循环"""
        self._running = True
        while self._running:
            cmd = self._get_next_command()

            if cmd.command == 'stop':
                break
            elif cmd.command == 'seek_precise':
                self._cancel_token.reset()
                success = self._decoder.seek_to_precise(
                    cmd.timestamp_ms,
                    self._cancel_token
                )
                if success:
                    self.seek_completed.emit(self._track_index, cmd.timestamp_ms)
            # ... 其他命令处理 ...

    def _get_next_command(self) -> DecodeCommand:
        with QMutexLocker(self._mutex):
            while not self._command_queue and self._running:
                self._wait_condition.wait(self._mutex)
            return self._command_queue.pop(0) if self._command_queue else DecodeCommand('stop')
```

#### 3. DecoderPoolAsync (替换 DecoderPool)

```python
# player/core/decoder_pool_async.py

class DecoderPoolAsync(QObject):
    """异步多轨道解码器池"""

    # 信号
    frame_ready = Signal()  # 所有轨道解码完成 (纹理已上传)
    duration_changed = Signal(int)
    position_changed = Signal(int)
    seek_started = Signal(int)  # (target_ms) seek 开始
    seek_completed = Signal(int)  # (actual_ms) seek 完成
    seek_cancelled = Signal()
    error_occurred = Signal(int, str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._workers: list[DecodeWorker | None] = [None] * self.MAX_TRACKS
        self._async_manager = AsyncOperationManager(self)

    def probe_file_async(self, path: str, callback) -> int:
        """异步探测文件"""
        return self._async_manager.submit(
            self._probe_file_impl, path, callback
        )

    def seek_to_precise(self, position_ms: int):
        """精确 seek (可取消)"""
        # 取消之前的 seek
        for worker in self._workers:
            if worker:
                worker.cancel_current()

        self.seek_started.emit(position_ms)

        # 投递新的 seek 命令
        for worker in self._workers:
            if worker:
                worker.post_command(DecodeCommand('seek_precise', position_ms))

    def cancel_seek(self):
        """取消当前 seek"""
        for worker in self._workers:
            if worker:
                worker.cancel_current()
        self.seek_cancelled.emit()
```

#### 4. TextureUploadScheduler (GL 线程调度)

```python
# player/core/texture_upload.py

class TextureUploadScheduler(QObject):
    """
    纹理上传调度器

    确保所有 OpenGL 操作都在 GL 上下文中执行。
    与 MultiTrackGLWidget 协作。
    """

    upload_requested = Signal(int)  # (track_index)

    def __init__(self, gl_widget: "MultiTrackGLWidget", parent=None):
        super().__init__(parent)
        self._gl_widget = gl_widget
        self._pending_uploads: set[int] = set()  # 待上传的 track_index

    def schedule_upload(self, track_index: int):
        """调度纹理上传 (可在任意线程调用)"""
        self._pending_uploads.add(track_index)
        # 请求 GL Widget 在下一帧绘制前执行上传
        QMetaObject.invokeMethod(
            self._gl_widget, "processPendingUploads",
            Qt.ConnectionType.QueuedConnection
        )

    def process_pending_uploads(self):
        """处理待上传帧 (必须在 GL 上下文调用)"""
        for track_index in list(self._pending_uploads):
            worker = self._workers[track_index]
            if worker and worker.has_pending_frame():
                worker.upload_pending_frame()  # 调用 native 上传
                self._pending_uploads.discard(track_index)
```

### OpenGL 上下文处理

#### 硬解 ZeroCopy

```
解码线程:
  avcodec_receive_frame() → AVFrame (D3D11Texture2D NV12)

GL 线程 (TextureUploadScheduler.process_pending_uploads):
  1. wglDXLockObjectsNV() - 锁定共享资源
  2. D3D11 VideoProcessorBlt() - GPU NV12→RGBA
  3. wglDXUnlockObjectsNV() - 解锁
  4. texture_id 可用于渲染
```

**关键**: WGL 互操作必须在 GL 线程执行，但不需要显式 GL 上下文切换

#### 软解帧上传

```
解码线程:
  avcodec_receive_frame() → AVFrame (YUV420P CPU 内存)

GL 线程:
  1. CPU YUV→RGBA 转换 (已在 decode_frame 中完成)
  2. glTexSubImage2D() 上传
```

**优化方案**: 可考虑使用 PBO (Pixel Buffer Object) 异步上传

```cpp
// 使用 PBO 异步上传
glGenBuffers(2, pbo_ids);
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_ids[0]);
glBufferData(GL_PIXEL_UNPACK_BUFFER, frame_size, nullptr, GL_STREAM_DRAW);

// 上传时
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_ids[current_pbo]);
glTexSubImage2D(...);  // 非阻塞, DMA 传输
```

### 取消机制详解

#### seek_to_precise 取消流程

```
用户拖动滑块:
  1. [0ms] seek_to_precise(1000) 开始
  2. [50ms] 用户继续拖动 → cancel_current()
  3. [50ms] CancelToken.cancel() 设置标志
  4. [51ms] 解码循环检测到 is_cancelled()
  5. [51ms] seek_to_precise 返回 false
  6. [52ms] seek_to_precise(1200) 开始
```

#### find_stream_info 取消

虽然技术上可以检查取消标志，但 `avformat_find_stream_info` 是一个整体操作：
- 一旦开始就必须完成才能获得有效的流信息
- 中途中断会导致 `AVFormatContext` 处于不一致状态

**结论**: `find_stream_info` 不支持取消，但应该异步执行避免阻塞 UI

### 接口变更总结

#### Native 层新增

| 接口 | 说明 |
|------|------|
| `CancelToken` 类 | 取消令牌，原子布尔标志 |
| `HardwareDecoder::decode_next_frame(CancelToken&)` | 可取消解码 |
| `HardwareDecoder::seek_to_precise(int64_t, CancelToken&)` | 可取消精确 seek |
| `HardwareDecoder::has_pending_frame()` | 检查是否有待上传帧 |
| `HardwareDecoder::upload_pending_frame()` | 上传已解码帧到纹理 |

#### Python 层新增

| 类 | 说明 |
|------|------|
| `AsyncOperationManager` | 异步任务管理器 |
| `DecodeWorker` | 解码线程 worker |
| `DecoderPoolAsync` | 异步解码器池 (替换 DecoderPool) |
| `TextureUploadScheduler` | GL 纹理上传调度器 |

### 迁移路径

#### 阶段 1: Native 层扩展 (不破坏现有接口)

1. 添加 `CancelToken` 类
2. 为 `HardwareDecoder` 添加可取消接口重载
3. 修改 `seek_frame_precise` 内部实现，支持取消检查点

#### 阶段 2: Python 异步框架

1. 实现 `AsyncOperationManager`
2. 实现 `DecodeWorker` 线程
3. 实现 `TextureUploadScheduler`

#### 阶段 3: DecoderPool 重构

1. 创建 `DecoderPoolAsync` 替换 `DecoderPool`
2. 修改 UI 层使用新的异步 API
3. 添加取消按钮/手势支持

### 风险与缓解

| 风险 | 缓解措施 |
|------|---------|
| 多线程 GL 上下文竞争 | 所有 GL 操作统一由 TextureUploadScheduler 调度 |
| 取消时资源泄漏 | CancelToken 析构时自动清理，RAII 模式 |
| 线程安全 | 使用 QMutex/QWaitCondition，避免数据竞争 |
| 性能回退 | 帧队列大小限制，避免内存暴涨 |

### 测试要点

1. **取消正确性**: 快速连续 seek 时，确保只有最后一个生效
2. **GL 上下文**: 验证所有纹理操作都在正确的线程
3. **内存泄漏**: 检查取消后的 AVFrame 是否正确释放
4. **性能**: 对比异步前后的 seek 响应时间
