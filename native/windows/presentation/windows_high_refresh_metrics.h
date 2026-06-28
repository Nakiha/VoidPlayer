#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vr {

struct WindowsHighRefreshMetricsSnapshot {
    bool gate_supported = false;
    int64_t display_hz = 60;
    int64_t present_interval_p95_us = 0;
    int64_t composite_p95_us = 0;
    int64_t draw_p95_us = 0;
    int64_t present_block_p95_us = 0;
    int64_t acquire_wait_p95_us = 0;
    int64_t interaction_input_to_present_p95_us = 0;
    int64_t drop_rate_x1000 = 0;
    uint64_t source_projection_reuse_count = 0;
    uint64_t viewport_redraw_during_projection_count = 0;
    uint64_t overlay_layer_raster_count = 0;
    uint64_t overlay_layer_upload_count = 0;
    uint64_t overlay_layer_reuse_count = 0;
    int64_t overlay_composite_p95_us = 0;
    int64_t overlay_raster_p95_us = 0;
    int64_t overlay_upload_p95_us = 0;
    std::string gate_last_result = "not-run";
};

std::string evaluate_windows_high_refresh_gate(
    const WindowsHighRefreshMetricsSnapshot& snapshot,
    bool source_projection_active,
    bool overlay_active);

class WindowsHighRefreshMetrics {
public:
    void reset(int64_t display_hz);
    void set_display_hz(int64_t display_hz);
    void record_present_interval_us(int64_t value);
    void record_composite_us(int64_t value);
    void record_draw_us(int64_t value);
    void record_present_block_us(int64_t value);
    void record_acquire_wait_us(int64_t value);
    void record_interaction_input_to_present_us(int64_t value);
    void record_drop();
    void record_source_projection_reuse();
    void record_viewport_redraw_during_projection();
    void record_overlay_layer_raster();
    void record_overlay_layer_upload();
    void record_overlay_layer_reuse();
    void record_overlay_composite_us(int64_t value);
    void record_overlay_raster_us(int64_t value);
    void record_overlay_upload_us(int64_t value);
    WindowsHighRefreshMetricsSnapshot snapshot() const;

private:
    static int64_t percentile95(std::vector<int64_t> values);

    int64_t display_hz_ = 60;
    uint64_t present_count_ = 0;
    uint64_t drop_count_ = 0;
    uint64_t source_projection_reuse_count_ = 0;
    uint64_t viewport_redraw_during_projection_count_ = 0;
    uint64_t overlay_layer_raster_count_ = 0;
    uint64_t overlay_layer_upload_count_ = 0;
    uint64_t overlay_layer_reuse_count_ = 0;
    std::vector<int64_t> present_interval_us_;
    std::vector<int64_t> composite_us_;
    std::vector<int64_t> draw_us_;
    std::vector<int64_t> present_block_us_;
    std::vector<int64_t> acquire_wait_us_;
    std::vector<int64_t> interaction_input_to_present_us_;
    std::vector<int64_t> overlay_composite_us_;
    std::vector<int64_t> overlay_raster_us_;
    std::vector<int64_t> overlay_upload_us_;
};

} // namespace vr
