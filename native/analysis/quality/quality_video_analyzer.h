#pragma once

#include "analysis/quality/quality_metrics.h"

#include <cstdint>
#include <functional>
#include <string>

namespace vr::analysis::quality {

enum class QualityComputeBackend {
    Cpu,
    Wgpu,
    Auto,
};

enum QualityMetricFlag : uint32_t {
    QualityMetricBlockiness = 1u << 0,
    QualityMetricBanding = 1u << 1,
    QualityMetricBlur = 1u << 2,
    QualityMetricNoise = 1u << 3,
    QualityMetricFlicker = 1u << 4,
};

constexpr uint32_t kQualitySpatialMetricMask =
    QualityMetricBlockiness |
    QualityMetricBanding |
    QualityMetricBlur |
    QualityMetricNoise;
constexpr uint32_t kQualityAllMetricMask =
    kQualitySpatialMetricMask | QualityMetricFlicker;

inline constexpr const char* kQualityAnalysisCancelledError =
    "quality analysis cancelled";

enum class QualityAnalysisPhase {
    Opening,
    Decoding,
    Finalizing,
};

struct QualityAnalysisProgress {
    QualityAnalysisPhase phase = QualityAnalysisPhase::Opening;
    uint64_t packet_count = 0;
    uint64_t packet_bytes = 0;
    uint64_t decoded_frames = 0;
    uint64_t sampled_frames = 0;
    int64_t pts_us = 0;
    int64_t duration_us = 0;
    bool has_pts = false;
    bool has_duration = false;
};

constexpr bool quality_metric_enabled(uint32_t mask,
                                      QualityMetricFlag metric) {
    return (mask & static_cast<uint32_t>(metric)) != 0;
}

struct QualityVideoAnalyzerOptions {
    int64_t sample_interval_us = 1'000'000;
    uint32_t max_samples = 0;
    QualityComputeBackend backend = QualityComputeBackend::Cpu;
    QualityCpuMode cpu_mode = QualityCpuMode::Auto;
    uint32_t decode_threads = 0;
    uint32_t cpu_workers = 0;
    uint32_t cpu_in_flight = 0;
    uint32_t metric_mask = kQualityAllMetricMask;
    bool collect_spatial_regions = true;
    bool collect_tile_metrics = false;
    std::function<bool()> cancel_requested;
    std::function<void(const QualityAnalysisProgress&)> progress_callback;
};

// Normalizes decoder output into the protocol order consumed by clients.
// The stable sort preserves decode order for frames sharing a timestamp, then
// sample_index is reassigned so it remains a contiguous timeline index.
void normalize_quality_timeline_order(QualityReport& report);

bool analyze_video_quality(const std::string& video_path,
                           const QualityVideoAnalyzerOptions& options,
                           QualityReport& report,
                           std::string& error);

}  // namespace vr::analysis::quality
