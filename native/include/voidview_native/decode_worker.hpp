#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <queue>

#include "voidview_native/hardware_decoder.hpp"
#include "voidview_native/frame_queue.hpp"

namespace voidview {

/**
 * 解码命令类型
 */
enum class DecodeCommandType {
    NONE = 0,
    SEEK_KEYFRAME,      // 快速 seek (关键帧)
    SEEK_PRECISE,       // 精确 seek (帧级)
    DECODE_FRAME,       // 解码下一帧 (填充帧缓冲)
    FILL_BUFFER,        // 填充帧缓冲直到满
    STOP                // 停止线程
};

/**
 * 解码命令
 */
struct DecodeCommand {
    DecodeCommandType type = DecodeCommandType::NONE;
    int64_t timestamp_ms = 0;   // for seek commands

    DecodeCommand() = default;
    DecodeCommand(DecodeCommandType t, int64_t ts = 0) : type(t), timestamp_ms(ts) {}
};

/**
 * 解码结果回调
 * @param track_index 轨道索引
 * @param success 是否成功
 * @param pts_ms 当前帧时间戳
 */
using DecodeCallback = std::function<void(int track_index, bool success, int64_t pts_ms)>;

/**
 * 后台解码线程
 *
 * 完全在 C++ 层运行，不涉及 Python GIL。
 * 通过回调函数通知 Python 结果。
 *
 * 线程安全：
 * - 命令提交通过 mutex 保护的队列
 * - 取消操作通过 atomic bool
 * - 回调时需要 acquire GIL
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
     * 取消当前操作
     */
    void cancel();

    /**
     * 停止线程
     */
    void stop();

    /**
     * 从帧队列取一帧 (带超时)
     * @param timeout_ms 超时时间，-1 表示无限等待
     * @return 帧的 PTS (ms)，失败返回 -1
     */
    int64_t pop_frame(int timeout_ms = -1);

    /**
     * 获取帧队列大小
     */
    size_t frame_queue_size() const;

    /**
     * 清空帧队列
     */
    void clear_frame_queue();

    /**
     * 获取帧队列
     */
    FrameQueue* frame_queue() { return &frame_queue_; }

    /**
     * 获取关联的解码器
     */
    HardwareDecoder* decoder() const { return decoder_; }

    /**
     * 获取轨道索引
     */
    int track_index() const { return track_index_; }

private:
    void worker_loop();
    void execute_command(const DecodeCommand& cmd);
    void notify_callback(bool success, int64_t pts_ms);
    void push_command(const DecodeCommand& cmd);
    void fill_frame_buffer();  // 填充帧缓冲

    HardwareDecoder* decoder_;
    int track_index_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::queue<DecodeCommand> command_queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};

    DecodeCallback callback_;

    FrameQueue frame_queue_{12};  // 帧缓冲队列
};

} // namespace voidview
