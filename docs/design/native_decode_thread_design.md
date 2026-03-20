# Native 后台解码线程设计 (方案 B)

## 背景

### 问题
- 用户拖动进度条 seek 时，UI 会卡顿 1-2 秒
- 原因：`seek_to_precise()` 需要逐帧解码，虽然是 Python ThreadPoolExecutor 调用，但 pybind11 调用仍持有 GIL
- 尝试在 C++ 层释放 GIL 后崩溃：Python 对象（HardwareDecoder, CancelToken）在 GIL 释放期间可能被移动/回收

### 为什么 Python 端无法解决
| 方案 | 问题 |
|------|------|
| `py::gil_scoped_release` | Python 对象访问不安全，导致 ACCESS_VIOLATION |
| Python QThread | 仍然持有 GIL，只是在不同线程 |
| asyncio | PySide6 集成复杂，且底层 native 调用仍阻塞 |

### 方案 B 核心思路
**在 C++ 层创建完全独立的后台解码线程**，与 Python 完全解耦：
- Python 只负责：发命令、收结果、上传纹理（在 GL 线程）
- C++ 后台线程：执行阻塞的 FFmpeg 操作，不涉及 Python GIL

## 架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                        Python 主线程 (UI + GL)                        │
├──────────────────────────────────────────────────────────────────────┤
│  DecoderPool                                                          │
│  ├── seek_to_precise(position_ms)  → 提交任务，立即返回               │
│  ├── _on_frame_ready()             ← 回调：纹理已准备好               │
│  └── _upload_textures()            → 在 GL 上下文上传纹理             │
└────────────────────────────┬─────────────────────────────────────────┘
                             │ 纯 C++ 回调 (无 GIL)
                             ▼
┌──────────────────────────────────────────────────────────────────────┐
│                     Native 后台解码线程 (C++)                         │
├──────────────────────────────────────────────────────────────────────┤
│  DecodeWorker (每个轨道一个)                                          │
│  ├── std::thread _thread          → 独立 C++ 线程                    │
│  ├── std::atomic<bool> _cancel    → 取消标志                         │
│  ├── 命令队列                        → seek/decode 命令               │
│  └── 回调函数                        → 通知 Python 完成               │
│                                                                       │
│  解码循环:                                                            │
│  1. 从队列取命令                                                      │
│  2. 执行 seek_to_precise / decode (阻塞，但不影响 UI)                 │
│  3. 完成后通过回调通知 Python                                         │
└──────────────────────────────────────────────────────────────────────┘
```

## C++ 接口设计

### DecodeWorker 类

```cpp
// native/include/voidview_native/decode_worker.hpp

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <queue>

namespace voidview {

/**
 * 解码命令
 */
struct DecodeCommand {
    enum Type {
        NONE = 0,
        SEEK_KEYFRAME,      // 快速 seek (关键帧)
        SEEK_PRECISE,       // 精确 seek (帧级)
        DECODE_FRAME,       // 解码下一帧
        DECODE_LOOP,        // 持续解码 (播放模式)
        STOP                // 停止线程
    };

    Type type = NONE;
    int64_t timestamp_ms = 0;   // for seek commands
};

/**
 * 解码结果回调
 */
using DecodeCallback = std::function<void(int track_index, bool success, int64_t pts_ms)>;

/**
 * 后台解码线程
 *
 * 完全在 C++ 层运行，不涉及 Python GIL。
 * 通过回调函数通知 Python 结果。
 */
class DecodeWorker {
public:
    /**
     * @param decoder 关联的硬件解码器 (不持有所有权)
     * @param track_index 轨道索引 (用于回调)
     */
    DecodeWorker(HardwareDecoder* decoder, int track_index);
    ~DecodeWorker();

    // 禁止拷贝
    DecodeWorker(const DecodeWorker&) = delete;
    DecodeWorker& operator=(const DecodeWorker&) = delete;

    /**
     * 设置回调函数 (在 Python 端调用)
     * 回调会在工作线程中执行，需要通过 Qt 信号转发到主线程
     */
    void set_callback(DecodeCallback callback);

    /**
     * 提交 seek 命令 (非阻塞)
     * 如果有正在执行的命令，会被取消
     */
    void seek_keyframe(int64_t timestamp_ms);
    void seek_precise(int64_t timestamp_ms);

    /**
     * 提交解码命令 (非阻塞)
     */
    void decode_frame();

    /**
     * 开始持续解码 (播放模式)
     */
    void start_decode_loop();

    /**
     * 停止持续解码
     */
    void stop_decode_loop();

    /**
     * 取消当前操作
     */
    void cancel();

    /**
     * 停止线程
     */
    void stop();

    /**
     * 检查是否有已解码帧等待上传
     */
    bool has_pending_frame() const;

    /**
     * 获取关联的解码器
     */
    HardwareDecoder* decoder() const { return decoder_; }

private:
    void worker_loop();
    void execute_command(const DecodeCommand& cmd);
    void notify_callback(bool success, int64_t pts_ms);

    HardwareDecoder* decoder_;
    int track_index_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;

    std::queue<DecodeCommand> command_queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> decode_loop_{false};

    DecodeCallback callback_;
};

} // namespace voidview
```

### 实现要点

```cpp
// native/src/decode_worker.cpp

void DecodeWorker::worker_loop() {
    while (running_) {
        DecodeCommand cmd;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !command_queue_.empty() || !running_;
            });

            if (!running_) break;

            if (!command_queue_.empty()) {
                cmd = command_queue_.front();
                command_queue_.pop();
            } else if (decode_loop_) {
                cmd.type = DecodeCommand::DECODE_FRAME;
            } else {
                continue;
            }
        }

        // 重置取消标志
        cancelled_ = false;

        execute_command(cmd);
    }
}

void DecodeWorker::execute_command(const DecodeCommand& cmd) {
    switch (cmd.type) {
        case DecodeCommand::SEEK_PRECISE:
            if (decoder_->seek_to_precise_internal(cmd.timestamp_ms, cancelled_)) {
                notify_callback(true, cmd.timestamp_ms);
            } else if (!cancelled_) {
                notify_callback(false, 0);
            }
            break;

        case DecodeCommand::DECODE_FRAME:
            if (decoder_->decode_frame_internal(cancelled_)) {
                notify_callback(true, decoder_->current_pts_ms());
            }
            break;

        // ... 其他命令
    }
}

void DecodeWorker::seek_precise(int64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 取消当前操作
    cancelled_ = true;

    // 清空队列中的旧 seek 命令
    std::queue<DecodeCommand> new_queue;
    while (!command_queue_.empty()) {
        auto& front = command_queue_.front();
        if (front.type != DecodeCommand::SEEK_PRECISE &&
            front.type != DecodeCommand::SEEK_KEYFRAME) {
            new_queue.push(front);
        }
        command_queue_.pop();
    }
    command_queue_ = std::move(new_queue);

    // 添加新命令
    command_queue_.push({DecodeCommand::SEEK_PRECISE, timestamp_ms});
    cv_.notify_one();
}
```

### HardwareDecoder 改动

需要将解码逻辑拆分为两步：

```cpp
class HardwareDecoder {
public:
    // 现有接口保持不变...

    // === 内部接口 (供 DecodeWorker 调用) ===

    /**
     * 内部解码 (不涉及 GL 上传)
     * 解码后设置 has_pending_frame_ = true
     */
    bool decode_frame_internal(std::atomic<bool>& cancel);

    /**
     * 内部精确 seek (可取消)
     */
    bool seek_to_precise_internal(int64_t timestamp_ms, std::atomic<bool>& cancel);

    // has_pending_frame() 和 upload_pending_frame() 已存在
};
```

## Python 绑定

```cpp
// native/src/bindings.cpp

// DecodeWorker 绑定
py::class_<voidview::DecodeWorker>(m, "DecodeWorker")
    .def(py::init<voidview::HardwareDecoder*, int>(),
         py::arg("decoder"), py::arg("track_index"))

    .def("set_callback", [](voidview::DecodeWorker& self, py::function callback) {
        // 包装 Python 回调，注意 GIL
        self.set_callback([callback](int track_idx, bool success, int64_t pts_ms) {
            py::gil_scoped_acquire gil;
            try {
                callback(track_idx, success, pts_ms);
            } catch (py::error_already_set& e) {
                // 记录错误但不崩溃
            }
        });
    })

    .def("seek_keyframe", &voidview::DecodeWorker::seek_keyframe,
         py::arg("timestamp_ms"))
    .def("seek_precise", &voidview::DecodeWorker::seek_precise,
         py::arg("timestamp_ms"))
    .def("decode_frame", &voidview::DecodeWorker::decode_frame)
    .def("start_decode_loop", &voidview::DecodeWorker::start_decode_loop)
    .def("stop_decode_loop", &voidview::DecodeWorker::stop_decode_loop)
    .def("cancel", &voidview::DecodeWorker::cancel)
    .def("stop", &voidview::DecodeWorker::stop)
    .def("has_pending_frame", &voidview::DecodeWorker::has_pending_frame)
    .def_property_readonly("decoder", &voidview::DecodeWorker::decoder);
```

## Python 端改动

### DecoderPool 修改

```python
# player/core/decoder_pool.py

class DecoderPool(QObject):
    frame_ready = Signal()
    seek_completed = Signal(int)  # position_ms

    def __init__(self, parent=None):
        super().__init__(parent)
        self._workers: list[voidview_native.DecodeWorker | None] = [None] * MAX_TRACKS

    def add_track(self, index: int, path: str) -> bool:
        # ... 创建 HardwareDecoder ...

        # 创建 DecodeWorker
        worker = voidview_native.DecodeWorker(decoder, index)
        worker.set_callback(self._on_decode_callback)
        self._workers[index] = worker
        return True

    def _on_decode_callback(self, track_index: int, success: bool, pts_ms: int):
        """解码完成回调 (从 C++ 线程调用，需转发到主线程)"""
        # 通过 Qt 信号转发到主线程
        QMetaObject.invokeMethod(
            self, "_handle_decode_result",
            Qt.ConnectionType.QueuedConnection,
            Q_ARG(int, track_index),
            Q_ARG(bool, success),
            Q_ARG(int, pts_ms)
        )

    @Slot(int, bool, int)
    def _handle_decode_result(self, track_index: int, success: bool, pts_ms: int):
        """在主线程处理解码结果"""
        if success:
            # 上传纹理 (在 GL 上下文)
            worker = self._workers[track_index]
            if worker and worker.has_pending_frame():
                worker.decoder().upload_pending_frame()

        # 检查是否所有轨道都完成
        self._check_all_tracks_ready()

    def seek_to_precise(self, position_ms: int):
        """精确 seek - 非阻塞"""
        for worker in self._workers:
            if worker:
                worker.seek_precise(position_ms)
```

## 线程安全要点

| 操作 | 线程 | GIL | GL 上下文 |
|------|------|-----|-----------|
| `DecodeWorker.seek_precise()` | Python 主线程 | 持有 | 不需要 |
| `DecodeWorker::worker_loop()` | C++ 工作线程 | **不持有** | 不需要 |
| `decoder.decode_frame_internal()` | C++ 工作线程 | **不持有** | 不需要 |
| `decoder.upload_pending_frame()` | Python 主线程 | 持有 | **需要** |
| 回调函数 | C++ 工作线程 | `gil_scoped_acquire` | 不需要 |
| `Signal.emit()` | 任意线程 | 安全 | 不需要 |

## 实现步骤

### 阶段 1: C++ DecodeWorker
1. 创建 `DecodeWorker` 类
2. 修改 `HardwareDecoder`，拆分 `decode_frame_internal` 和 `upload_pending_frame`
3. 添加 Python 绑定
4. 单元测试

### 阶段 2: Python 集成
1. 修改 `DecoderPool`，用 `DecodeWorker` 替换直接调用
2. 测试 seek 是否阻塞 UI
3. 测试播放是否正常

### 阶段 3: 优化
1. 帧队列预缓冲
2. 取消优化（快速连续 seek）
3. 性能测试

## 风险

| 风险 | 缓解 |
|------|------|
| C++ 线程访问已释放的 decoder | `DecodeWorker` 只持有裸指针，Python 端必须保证 decoder 生命周期 |
| 回调中访问 Python 对象崩溃 | 使用 `gil_scoped_acquire`，且通过 Qt 信号转发到主线程 |
| 多线程竞争 | 使用 `std::mutex` 保护命令队列 |
