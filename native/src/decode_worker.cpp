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

void DecodeWorker::start_decode_loop() {
    std::lock_guard<std::mutex> lock(mutex_);
    decode_loop_ = true;
    cancelled_ = false;
    cv_.notify_one();
    VV_DEBUG("DecodeWorker::start_decode_loop for track {}", track_index_);
}

void DecodeWorker::stop_decode_loop() {
    std::lock_guard<std::mutex> lock(mutex_);
    decode_loop_ = false;
    cancelled_ = true;
    VV_DEBUG("DecodeWorker::stop_decode_loop for track {}", track_index_);
}

void DecodeWorker::cancel() {
    cancelled_ = true;
    VV_DEBUG("DecodeWorker::cancel for track {}", track_index_);
}

void DecodeWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        decode_loop_ = false;
    }
    cv_.notify_one();
    VV_DEBUG("DecodeWorker::stop for track {}", track_index_);
}

bool DecodeWorker::has_pending_frame() const {
    return decoder_ && decoder_->has_pending_frame();
}

void DecodeWorker::worker_loop() {
    VV_DEBUG("DecodeWorker thread started for track {}", track_index_);

    while (running_) {
        DecodeCommand cmd;
        cmd.type = DecodeCommandType::NONE;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !command_queue_.empty() || !running_ || decode_loop_;
            });

            if (!running_) break;

            if (!command_queue_.empty()) {
                cmd = command_queue_.front();
                command_queue_.pop();
            } else if (decode_loop_) {
                cmd.type = DecodeCommandType::DECODE_FRAME;
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
            // Use internal method that doesn't do GL upload
            bool success = decoder_->seek_to_precise_internal(cmd.timestamp_ms);
            if (!cancelled_) {
                notify_callback(success, decoder_->get_current_pts_ms());
            }
            break;
        }

        case DecodeCommandType::DECODE_FRAME: {
            VV_TRACE("DecodeWorker executing DECODE_FRAME");
            // Use internal method that doesn't do GL upload
            bool success = decoder_->decode_frame_internal();
            if (!cancelled_) {
                notify_callback(success, decoder_->get_current_pts_ms());
            }
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
