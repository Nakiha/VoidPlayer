#pragma once
#include <vector>
#include <mutex>
#include <optional>
#include <cstdint>
#include <memory>

namespace vr {

struct TextureFrame {
    int64_t pts_us = 0;
    int64_t duration_us = 0;
    int width = 0;
    int height = 0;
    bool is_ref = false;
    void* texture_handle = nullptr;
    // Owns CPU-side RGBA data; shared_ptr enables safe cross-thread sharing
    // and automatic cleanup when all references are gone.
    std::shared_ptr<std::vector<uint8_t>> cpu_data;

    // Hardware decode metadata (D3D11VA NV12)
    bool is_nv12 = false;               // true if NV12 D3D11VA frame
    int texture_array_index = 0;        // Texture2DArray slice index

    // Holds a reference to the underlying AVFrame/hw buffer. Prevents the
    // decoder from reusing the frame pool slot while the render thread
    // still has a TextureFrame pointing to it. Released automatically via
    // shared_ptr deleter (calls av_frame_free or av_buffer_unref).
    std::shared_ptr<void> hw_frame_ref;
};

class BidiRingBuffer {
public:
    explicit BidiRingBuffer(size_t forward_depth = 4, size_t backward_depth = 2);

    // Write side (Decode thread)
    bool push(TextureFrame frame);

    // Read side (Render thread)
    std::optional<TextureFrame> peek(int offset = 0) const;
    bool advance();    // read_idx++
    bool retreat();    // read_idx--
    bool can_advance() const;
    bool can_retreat() const;

    // State
    size_t capacity() const { return capacity_; }
    /// Max push-able frames: reserves backward_depth slots behind read_idx
    /// so retreat never lands on a push-overwritten slot.
    size_t max_count() const { return capacity_ - backward_depth_; }
    size_t forward_count() const;
    size_t backward_count() const;
    size_t total_count() const;
    bool empty() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<TextureFrame> ring_;
    size_t capacity_;
    size_t forward_depth_;
    size_t backward_depth_;
    size_t write_idx_ = 0;
    size_t read_idx_ = 0;
    size_t count_ = 0;
    size_t retreated_ = 0;       // How many times we've retreated past the last advance position
    size_t total_advanced_ = 0;  // Total advances since last clear (limits valid retreat range)
};

} // namespace vr
