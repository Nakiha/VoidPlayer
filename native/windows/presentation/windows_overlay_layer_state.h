#pragma once

#include <cstdint>
#include <string>

namespace vr {

struct WindowsOverlayLayerSignature {
    uint64_t primitive_generation = 0;
    uint64_t track_signature = 0;
    uint32_t target_class = 0;
    uint32_t sdr_white_scale_x1000 = 1000;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t fill_rect_count = 0;
    uint32_t outline_rect_count = 0;
    uint32_t motion_line_count = 0;

    bool valid() const;
    bool operator==(const WindowsOverlayLayerSignature& other) const;
    bool operator!=(const WindowsOverlayLayerSignature& other) const {
        return !(*this == other);
    }
};

struct WindowsOverlayLayerStateSnapshot {
    bool active = false;
    std::string mode = "inactive";
    uint64_t generation = 0;
    uint64_t committed_generation = 0;
    uint64_t pending_generation = 0;
    uint64_t texture_count = 0;
    uint64_t bytes = 0;
    uint64_t raster_count = 0;
    uint64_t upload_count = 0;
    uint64_t reuse_count = 0;
    uint64_t composite_count = 0;
    uint64_t miss_count = 0;
    uint64_t backpressure_count = 0;
    std::string fallback_reason = "none";
    std::string last_error = "none";
};

class WindowsOverlayLayerCacheState {
public:
    void reset(const std::string& reason);
    bool prepare(const WindowsOverlayLayerSignature& signature,
                 uint64_t bytes);
    void reuse();
    void composite();
    void miss(const std::string& reason);
    void backpressure(const std::string& reason);
    void fail(const std::string& reason);

    const WindowsOverlayLayerSignature& signature() const {
        return signature_;
    }
    WindowsOverlayLayerStateSnapshot snapshot() const;

private:
    WindowsOverlayLayerSignature signature_;
    WindowsOverlayLayerStateSnapshot snapshot_;
};

} // namespace vr
