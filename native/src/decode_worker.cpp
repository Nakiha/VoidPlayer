#include "voidview_native/decode_worker.hpp"
#include "voidview_native/logger.hpp"

namespace voidview {

DecodeWorker::DecodeWorker(HardwareDecoder* decoder, int track_index)
    : decoder_(decoder)
    , track_index_(track_index)
{
    running_ = true;
    thread_ = std::thread(&DecodeWorker::worker_loop, this);
    VV_DEBUG("DecodeWorker created for track {}", track_index);
}

DecodeWorker::~DecodeWorker() {
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
    VV_DEBUG("DecodeWorker destroyed for track {}", track_index_);
}

void DecodeWorker::set_callback(DecodeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void DecodeWorker::push_command(const DecodeCommand& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 取消当前操作
    cancelled_ = true;

    // 清空队列中的旧 seek 命令 (对于新的 seek 命令)
    if (cmd.type == DecodeCommandType::SEEK_PRECISE ||
        cmd.type == DecodeCommandType::SEEK_KEYFRAME) {
        std::queue<DecodeCommand> new_queue;
        while (!command_queue_.empty()) {
            const auto& front = command_queue_.front();
            if (front.type != DecodeCommandType::SEEK_PRECISE &&
                front.type != DecodeCommandType::SEEK_KEYFRAME) {
                new_queue.push(front);
            }
            command_queue_.pop();
        }
        command_queue_ = std::move(new_queue);
    }

    // 添加新命令
    command_queue_.push(cmd);
    cv_.notify_one();
}

void DecodeWorker::seek_keyframe(int64_t timestamp_ms) {
    VV_DEBUG("DecodeWorker::seek_keyframe({}) for track {}", timestamp_ms, track_index_);
    push_command({DecodeCommandType::SEEK_KEYFRAME, timestamp_ms});
}

void DecodeWorker::seek_precise(int64_t timestamp_ms) {
    VV_DEBUG("DecodeWorker::seek_precise({}) for track {}", timestamp_ms, track_index_);
    push_command({DecodeCommandType::SEEK_PRECISE, timestamp_ms});
}

void DecodeWorker::decode_frame() {
    push_command({DecodeCommandType::DECODE_FRAME, 0});
}

void DecodeWorker::cancel() {
    cancelled_ = true;
    VV_DEBUG("DecodeWorker::cancel for track {}", track_index_);
}

void DecodeWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_one();
    VV_DEBUG("DecodeWorker::stop for track {}", track_index_);
}

int64_t DecodeWorker::pop_frame(int timeout_ms) {
    // 使用 move_next 从 future 队列取帧
    if (!frame_queue_.move_next()) {
        return -1;  // 没有可用帧
    }

    // 获取当前帧并设置到解码器进行纹理上传
    AVFrame* frame = frame_queue_.take_current();
    if (!frame) {
        return -1;
    }

    decoder_->set_pending_frame(frame);

    // 在 set_pending_frame 之后获取 PTS
    int64_t pts = decoder_->get_current_pts_ms();

    VV_TRACE("DecodeWorker::pop_frame: got frame from queue, pts={}ms, future_size={}",
             pts, frame_queue_.future_size());

    // 低水位触发填充
    if (frame_queue_.future_size() < 4 && !cancelled_ && running_) {
        std::lock_guard<std::mutex> lock(mutex_);
        command_queue_.push({DecodeCommandType::FILL_BUFFER, 0});
        cv_.notify_one();
    }

    return pts;
}

size_t DecodeWorker::frame_queue_size() const {
    size_t queue_size = frame_queue_.total_size();
    // 还要算上 decoder 中待处理的帧
    if (decoder_ && decoder_->has_pending_frame()) {
        queue_size += 1;
    }
    return queue_size;
}

void DecodeWorker::clear_frame_queue() {
    frame_queue_.clear();
}

int64_t DecodeWorker::next_frame() {
    // 前进一帧：从 future 队列取帧
    if (!frame_queue_.move_next()) {
        VV_TRACE("DecodeWorker::next_frame: no future frames available");
        return -1;
    }

    // 获取当前帧并设置到解码器
    AVFrame* frame = frame_queue_.take_current();
    if (!frame) {
        return -1;
    }

    decoder_->set_pending_frame(frame);
    int64_t pts = decoder_->get_current_pts_ms();

    VV_TRACE("DecodeWorker::next_frame: moved to next, pts={}ms, history={}, future={}",
             pts, frame_queue_.history_size(), frame_queue_.future_size());

    // 触发填充 future 队列
    if (frame_queue_.future_size() < 4 && !cancelled_ && running_) {
        std::lock_guard<std::mutex> lock(mutex_);
        command_queue_.push({DecodeCommandType::FILL_BUFFER, 0});
        cv_.notify_one();
    }

    return pts;
}

int64_t DecodeWorker::prev_frame() {
    // 后退一帧：从 history 队列取帧
    if (!frame_queue_.move_prev()) {
        VV_TRACE("DecodeWorker::prev_frame: no history frames available");
        return -1;
    }

    // 获取当前帧并设置到解码器
    // 使用 take_current_no_history 避免把从 history 取出的帧又保存回 history
    AVFrame* frame = frame_queue_.take_current_no_history();
    if (!frame) {
        return -1;
    }

    decoder_->set_pending_frame(frame);
    int64_t pts = decoder_->get_current_pts_ms();

    VV_TRACE("DecodeWorker::prev_frame: moved to prev, pts={}ms, history={}, future={}",
             pts, frame_queue_.history_size(), frame_queue_.future_size());

    return pts;
}

size_t DecodeWorker::history_size() const {
    return frame_queue_.history_size();
}

size_t DecodeWorker::future_size() const {
    return frame_queue_.future_size();
}

void DecodeWorker::fill_frame_buffer() {
    if (!decoder_) return;

    VV_TRACE("DecodeWorker::fill_frame_buffer for track {}, future_size={}/{}",
             track_index_, frame_queue_.future_size(), frame_queue_.future_capacity());

    int frames_decoded = 0;

    // 持续解码直到 future 队列满
    while (frame_queue_.future_size() < frame_queue_.future_capacity() && !cancelled_ && running_) {
        bool success = decoder_->decode_frame_internal();
        if (!success) {
            if (decoder_->is_eof()) {
                VV_DEBUG("DecodeWorker::fill_frame_buffer: EOF reached for track {}", track_index_);
                break;
            }
            // 错误处理
            VV_WARN("DecodeWorker::fill_frame_buffer: decode failed for track {}", track_index_);
            break;
        }

        // 从 decoder 取出解码帧，放入 future 队列
        AVFrame* frame = decoder_->take_pending_frame();
        if (frame) {
            frame_queue_.push_future(frame);
            frames_decoded++;
        }
    }

    VV_TRACE("DecodeWorker::fill_frame_buffer: decoded {} frames, future_size={}",
             frames_decoded, frame_queue_.future_size());

    // 通知回调（使用 decoder 的当前 PTS）
    if (!cancelled_ && frames_decoded > 0) {
        notify_callback(true, decoder_->get_current_pts_ms());
    } else if (!cancelled_) {
        notify_callback(false, decoder_->get_current_pts_ms());
    }
}

void DecodeWorker::worker_loop() {
    VV_DEBUG("DecodeWorker thread started for track {}", track_index_);

    while (running_) {
        DecodeCommand cmd;
        cmd.type = DecodeCommandType::NONE;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !command_queue_.empty() || !running_;
            });

            if (!running_) break;

            if (!command_queue_.empty()) {
                cmd = command_queue_.front();
                command_queue_.pop();
            } else {
                continue;
            }
        }

        // 重置取消标志
        cancelled_ = false;

        execute_command(cmd);
    }

    VV_DEBUG("DecodeWorker thread exiting for track {}", track_index_);
}

void DecodeWorker::execute_command(const DecodeCommand& cmd) {
    if (!decoder_) {
        notify_callback(false, 0);
        return;
    }

    switch (cmd.type) {
        case DecodeCommandType::SEEK_KEYFRAME: {
            VV_TRACE("DecodeWorker executing SEEK_KEYFRAME to {}ms", cmd.timestamp_ms);
            // 清空帧队列，避免旧帧干扰
            frame_queue_.clear();
            // Seek to keyframe, then decode one frame
            bool success = decoder_->seek_to_keyframe_internal(cmd.timestamp_ms);
            if (success && !cancelled_) {
                success = decoder_->decode_frame_internal();
            }
            if (!cancelled_) {
                notify_callback(success, decoder_->get_current_pts_ms());
            }
            break;
        }

        case DecodeCommandType::SEEK_PRECISE: {
            VV_TRACE("DecodeWorker executing SEEK_PRECISE to {}ms", cmd.timestamp_ms);
            // 清空帧队列，避免旧帧干扰
            frame_queue_.clear();
            // Use internal method that doesn't do GL upload
            bool success = decoder_->seek_to_precise_internal(cmd.timestamp_ms);
            if (!cancelled_) {
                notify_callback(success, decoder_->get_current_pts_ms());
            }
            break;
        }

        case DecodeCommandType::DECODE_FRAME: {
            VV_TRACE("DecodeWorker executing DECODE_FRAME");
            // 填充模式：持续解码直到队列满
            fill_frame_buffer();
            break;
        }

        case DecodeCommandType::FILL_BUFFER: {
            VV_TRACE("DecodeWorker executing FILL_BUFFER");
            fill_frame_buffer();
            break;
        }

        case DecodeCommandType::STOP:
            running_ = false;
            break;

        default:
            break;
    }
}

void DecodeWorker::notify_callback(bool success, int64_t pts_ms) {
    if (callback_) {
        try {
            callback_(track_index_, success, pts_ms);
        } catch (const std::exception& e) {
            VV_ERROR("DecodeWorker callback exception: {}", e.what());
        }
    }
}

} // namespace voidview
