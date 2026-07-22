#pragma once

#include "analysis/quality/quality_video_analyzer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vr::analysis::quality {

inline constexpr uint32_t kQualityEventSchemaVersion = 1;
inline constexpr const char* kQualityEventSchemaId = "quality-event-v1";
inline constexpr const char* kQualityEventPolicyVersion =
    "quality-candidate-policy-v1";

enum class QualityEventClassification {
    RelativeOutlier,
    SpatialCandidate,
};

enum class QualityEventThresholdKind {
    RobustRelative,
    SpatialDetection,
};

struct QualityEventThreshold {
    QualityEventThresholdKind kind =
        QualityEventThresholdKind::RobustRelative;
    double value = 0.0;
    double median = -1.0;
    double mad = -1.0;
    double p90 = -1.0;
    double robust_sigma = -1.0;
    double sigma_multiplier = -1.0;
};

struct QualityEvent {
    std::string metric;
    QualityEventClassification classification =
        QualityEventClassification::RelativeOutlier;
    uint64_t start_sample_index = 0;
    uint64_t end_sample_index = 0;
    uint64_t peak_sample_index = 0;
    int64_t start_pts_us = 0;
    int64_t end_pts_us = 0;
    int64_t peak_pts_us = 0;
    double peak_score = 0.0;
    uint32_t evidence_sample_count = 0;
    QualityEventThreshold threshold;
    bool has_spatial_region = false;
    QualitySpatialRegion spatial_region;
};

struct QualityEventAggregationOptions {
    uint32_t metric_mask = kQualityAllMetricMask;
    bool include_spatial_regions = true;
    uint32_t min_relative_samples = 5;
    double robust_sigma_multiplier = 3.0;
    int64_t minimum_merge_gap_us = 250'000;
};

/// Produces experimental candidate evidence, not calibrated pass/fail labels.
/// Relative candidates are robust within-video outliers. Banding regions from
/// the spatial detector are preserved as precise peak-frame evidence.
std::vector<QualityEvent> aggregate_quality_events(
    const QualityReport& report,
    const QualityEventAggregationOptions& options = {});

const char* quality_event_classification_name(
    QualityEventClassification classification);
const char* quality_event_threshold_kind_name(
    QualityEventThresholdKind kind);

}  // namespace vr::analysis::quality
