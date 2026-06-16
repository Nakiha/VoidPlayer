#include "windows/presentation/windows_high_refresh_metrics.h"

#include <algorithm>
#include <cmath>

namespace vr {
namespace {

constexpr size_t kMaxSamples = 512;

void append_bounded(std::vector<int64_t>& samples, int64_t value) {
    if (value < 0) {
        return;
    }
    if (samples.size() >= kMaxSamples) {
        samples.erase(samples.begin());
    }
    samples.push_back(value);
}

} // namespace

void WindowsHighRefreshMetrics::reset(int64_t display_hz) {
    display_hz_ = display_hz > 0 ? display_hz : 60;
    present_count_ = 0;
    drop_count_ = 0;
    source_projection_reuse_count_ = 0;
    viewport_redraw_during_projection_count_ = 0;
    overlay_layer_raster_count_ = 0;
    overlay_layer_upload_count_ = 0;
    overlay_layer_reuse_count_ = 0;
    present_interval_us_.clear();
    composite_us_.clear();
    acquire_wait_us_.clear();
    interaction_input_to_present_us_.clear();
    overlay_composite_us_.clear();
    overlay_raster_us_.clear();
    overlay_upload_us_.clear();
}

void WindowsHighRefreshMetrics::set_display_hz(int64_t display_hz) {
    if (display_hz > 0) {
        display_hz_ = display_hz;
    }
}

void WindowsHighRefreshMetrics::record_present_interval_us(int64_t value) {
    append_bounded(present_interval_us_, value);
    ++present_count_;
}

void WindowsHighRefreshMetrics::record_composite_us(int64_t value) {
    append_bounded(composite_us_, value);
}

void WindowsHighRefreshMetrics::record_acquire_wait_us(int64_t value) {
    append_bounded(acquire_wait_us_, value);
}

void WindowsHighRefreshMetrics::record_interaction_input_to_present_us(
    int64_t value) {
    append_bounded(interaction_input_to_present_us_, value);
}

void WindowsHighRefreshMetrics::record_drop() {
    ++drop_count_;
}

void WindowsHighRefreshMetrics::record_source_projection_reuse() {
    ++source_projection_reuse_count_;
}

void WindowsHighRefreshMetrics::record_viewport_redraw_during_projection() {
    ++viewport_redraw_during_projection_count_;
}

void WindowsHighRefreshMetrics::record_overlay_layer_raster() {
    ++overlay_layer_raster_count_;
}

void WindowsHighRefreshMetrics::record_overlay_layer_upload() {
    ++overlay_layer_upload_count_;
}

void WindowsHighRefreshMetrics::record_overlay_layer_reuse() {
    ++overlay_layer_reuse_count_;
}

void WindowsHighRefreshMetrics::record_overlay_composite_us(int64_t value) {
    append_bounded(overlay_composite_us_, value);
}

void WindowsHighRefreshMetrics::record_overlay_raster_us(int64_t value) {
    append_bounded(overlay_raster_us_, value);
}

void WindowsHighRefreshMetrics::record_overlay_upload_us(int64_t value) {
    append_bounded(overlay_upload_us_, value);
}

WindowsHighRefreshMetricsSnapshot WindowsHighRefreshMetrics::snapshot() const {
    WindowsHighRefreshMetricsSnapshot result;
    result.display_hz = display_hz_;
    result.gate_supported = display_hz_ >= 100;
    result.present_interval_p95_us = percentile95(present_interval_us_);
    result.composite_p95_us = percentile95(composite_us_);
    result.acquire_wait_p95_us = percentile95(acquire_wait_us_);
    result.interaction_input_to_present_p95_us =
        percentile95(interaction_input_to_present_us_);
    const auto total = present_count_ + drop_count_;
    result.drop_rate_x1000 =
        total > 0 ? static_cast<int64_t>((drop_count_ * 1000) / total) : 0;
    result.source_projection_reuse_count = source_projection_reuse_count_;
    result.viewport_redraw_during_projection_count =
        viewport_redraw_during_projection_count_;
    result.overlay_layer_raster_count = overlay_layer_raster_count_;
    result.overlay_layer_upload_count = overlay_layer_upload_count_;
    result.overlay_layer_reuse_count = overlay_layer_reuse_count_;
    result.overlay_composite_p95_us = percentile95(overlay_composite_us_);
    result.overlay_raster_p95_us = percentile95(overlay_raster_us_);
    result.overlay_upload_p95_us = percentile95(overlay_upload_us_);
    result.gate_last_result =
        evaluate_windows_high_refresh_gate(result, false, false);
    return result;
}

std::string evaluate_windows_high_refresh_gate(
    const WindowsHighRefreshMetricsSnapshot& snapshot,
    bool source_projection_active,
    bool overlay_active) {
    if (!snapshot.gate_supported || snapshot.display_hz < 100) {
        return "functional-only-low-refresh";
    }
    const auto refresh_period_us =
        snapshot.display_hz > 0
            ? static_cast<int64_t>(1000000 / snapshot.display_hz)
            : 16666;
    if (source_projection_active &&
        snapshot.viewport_redraw_during_projection_count != 0) {
        return "fail-viewport-redraw";
    }
    if (source_projection_active &&
        snapshot.source_projection_reuse_count == 0) {
        return "fail-source-cache-no-reuse";
    }
    if (overlay_active && snapshot.overlay_layer_reuse_count == 0) {
        return "fail-overlay-no-reuse";
    }
    if (snapshot.overlay_layer_raster_count != 0 &&
        snapshot.overlay_layer_reuse_count <=
            snapshot.overlay_layer_raster_count) {
        return "fail-overlay-no-reuse";
    }
    if (snapshot.present_interval_p95_us != 0 &&
        snapshot.present_interval_p95_us >
            static_cast<int64_t>(std::ceil(refresh_period_us * 1.5))) {
        return "fail-present-cadence";
    }
    if (snapshot.interaction_input_to_present_p95_us != 0 &&
        snapshot.interaction_input_to_present_p95_us >
            static_cast<int64_t>(std::ceil(refresh_period_us * 2.5))) {
        return "fail-input-latency";
    }
    if (snapshot.drop_rate_x1000 > 20) {
        return "fail-drop-rate";
    }
    return "pass";
}

int64_t WindowsHighRefreshMetrics::percentile95(
    std::vector<int64_t> values) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(
        std::ceil(static_cast<double>(values.size()) * 0.95) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

} // namespace vr
