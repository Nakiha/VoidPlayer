#include "analysis/quality/quality_event_aggregator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace vr::analysis::quality {
namespace {

struct MetricAccessor {
    const char* name;
    QualityMetricFlag flag;
    double FrameQualitySample::*score;
};

constexpr std::array<MetricAccessor, 5> kMetricAccessors{{
    {"blockiness", QualityMetricBlockiness,
     &FrameQualitySample::blockiness},
    {"banding", QualityMetricBanding, &FrameQualitySample::banding},
    {"blur", QualityMetricBlur, &FrameQualitySample::blur},
    {"noise", QualityMetricNoise, &FrameQualitySample::noise},
    {"flicker", QualityMetricFlicker, &FrameQualitySample::flicker},
}};

double percentile(const std::vector<double>& sorted, double quantile) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double position = std::clamp(quantile, 0.0, 1.0) *
                            static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = static_cast<size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

QualityEventThreshold make_relative_threshold(
    std::vector<double> values,
    double sigma_multiplier) {
    std::sort(values.begin(), values.end());
    const double median = percentile(values, 0.5);
    const double p90 = percentile(values, 0.9);
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const double value : values) {
        deviations.push_back(std::abs(value - median));
    }
    std::sort(deviations.begin(), deviations.end());
    const double mad = percentile(deviations, 0.5);
    const double robust_sigma = mad * 1.4826;

    QualityEventThreshold threshold;
    threshold.kind = QualityEventThresholdKind::RobustRelative;
    threshold.median = median;
    threshold.mad = mad;
    threshold.p90 = p90;
    threshold.robust_sigma = robust_sigma;
    threshold.sigma_multiplier = sigma_multiplier;
    threshold.value = std::max(
        p90, median + sigma_multiplier * robust_sigma);
    return threshold;
}

struct Candidate {
    const FrameQualitySample* sample = nullptr;
    double score = 0.0;
    double rank_score = 0.0;
    QualityEventClassification classification =
        QualityEventClassification::RelativeOutlier;
    QualityEventThreshold threshold;
    const QualitySpatialRegion* region = nullptr;
};

std::vector<const QualitySpatialRegion*> ordered_banding_regions(
    const FrameQualitySample& sample) {
    std::vector<const QualitySpatialRegion*> regions;
    for (const auto& region : sample.spatial_regions) {
        if (region.metric == "banding") {
            regions.push_back(&region);
        }
    }
    std::stable_sort(
        regions.begin(), regions.end(), [](const auto* left,
                                           const auto* right) {
            if (left->x != right->x) return left->x < right->x;
            if (left->y != right->y) return left->y < right->y;
            if (left->width != right->width) {
                return left->width < right->width;
            }
            if (left->height != right->height) {
                return left->height < right->height;
            }
            return left->score > right->score;
        });
    return regions;
}

double event_peak_rank(const QualityEvent& event) {
    if (event.has_spatial_region) {
        return event.spatial_region.score;
    }
    return event.peak_score;
}

void merge_candidate(QualityEvent& event, const Candidate& candidate) {
    event.end_sample_index = candidate.sample->sample_index;
    event.end_pts_us = candidate.sample->pts_us;
    ++event.evidence_sample_count;
    if (candidate.rank_score > event_peak_rank(event)) {
        event.peak_sample_index = candidate.sample->sample_index;
        event.peak_pts_us = candidate.sample->pts_us;
        event.peak_score = candidate.score;
        event.threshold = candidate.threshold;
        if (candidate.region) {
            event.has_spatial_region = true;
            event.spatial_region = *candidate.region;
        }
    }
}

double intersection_over_union(const QualitySpatialRegion& left,
                               const QualitySpatialRegion& right) {
    const int left_x2 = left.x + left.width;
    const int left_y2 = left.y + left.height;
    const int right_x2 = right.x + right.width;
    const int right_y2 = right.y + right.height;
    const int intersection_width =
        std::max(0, std::min(left_x2, right_x2) - std::max(left.x, right.x));
    const int intersection_height =
        std::max(0, std::min(left_y2, right_y2) - std::max(left.y, right.y));
    const double intersection =
        static_cast<double>(intersection_width) * intersection_height;
    const double union_area =
        static_cast<double>(left.width) * left.height +
        static_cast<double>(right.width) * right.height - intersection;
    return union_area > 0.0 ? intersection / union_area : 0.0;
}

QualityEvent event_from_candidate(const char* metric,
                                  const Candidate& candidate) {
    QualityEvent event;
    event.metric = metric;
    event.classification = candidate.classification;
    event.start_sample_index = candidate.sample->sample_index;
    event.end_sample_index = candidate.sample->sample_index;
    event.peak_sample_index = candidate.sample->sample_index;
    event.start_pts_us = candidate.sample->pts_us;
    event.end_pts_us = candidate.sample->pts_us;
    event.peak_pts_us = candidate.sample->pts_us;
    event.peak_score = candidate.score;
    event.evidence_sample_count = 1;
    event.threshold = candidate.threshold;
    if (candidate.region) {
        event.has_spatial_region = true;
        event.spatial_region = *candidate.region;
    }
    return event;
}

double merge_affinity(const QualityEvent& current,
                      const Candidate& candidate,
                      const QualitySpatialRegion* last_spatial_region,
                      int64_t merge_gap_us) {
    if (current.classification != candidate.classification ||
        candidate.sample->sample_index != current.end_sample_index + 1 ||
        candidate.sample->pts_us < current.end_pts_us ||
        candidate.sample->pts_us - current.end_pts_us > merge_gap_us) {
        return -1.0;
    }
    if (!last_spatial_region && !candidate.region) {
        return 1.0;
    }
    if (!last_spatial_region || !candidate.region) {
        return -1.0;
    }
    const double overlap = intersection_over_union(
        *last_spatial_region, *candidate.region);
    return overlap >= 0.10 ? overlap : -1.0;
}

}  // namespace

const char* quality_event_classification_name(
    QualityEventClassification classification) {
    switch (classification) {
    case QualityEventClassification::RelativeOutlier:
        return "relativeOutlier";
    case QualityEventClassification::SpatialCandidate:
        return "spatialCandidate";
    }
    return "relativeOutlier";
}

const char* quality_event_threshold_kind_name(
    QualityEventThresholdKind kind) {
    switch (kind) {
    case QualityEventThresholdKind::RobustRelative:
        return "robustRelative";
    case QualityEventThresholdKind::SpatialDetection:
        return "spatialDetection";
    }
    return "robustRelative";
}

std::vector<QualityEvent> aggregate_quality_events(
    const QualityReport& report,
    const QualityEventAggregationOptions& options) {
    std::vector<QualityEvent> events;
    std::vector<QualitySpatialRegion> last_spatial_regions;
    std::vector<bool> has_last_spatial_region;
    const int64_t merge_gap_us = std::max<int64_t>(
        options.minimum_merge_gap_us,
        report.sample_interval_us > 0
            ? report.sample_interval_us * 2
            : 0);

    for (const auto& metric : kMetricAccessors) {
        if (!quality_metric_enabled(options.metric_mask, metric.flag)) {
            continue;
        }
        std::vector<double> values;
        values.reserve(report.timeline.size());
        for (const auto& sample : report.timeline) {
            const double score = sample.*(metric.score);
            if (score >= 0.0 && std::isfinite(score)) {
                values.push_back(score);
            }
        }

        const bool relative_available =
            values.size() >= options.min_relative_samples;
        QualityEventThreshold relative_threshold;
        if (relative_available) {
            relative_threshold = make_relative_threshold(
                values, options.robust_sigma_multiplier);
        }

        std::vector<Candidate> candidates;
        candidates.reserve(report.timeline.size());
        for (const auto& sample : report.timeline) {
            const double score = sample.*(metric.score);
            if (score < 0.0 || !std::isfinite(score)) {
                continue;
            }

            std::vector<const QualitySpatialRegion*> regions;
            if (options.include_spatial_regions &&
                metric.flag == QualityMetricBanding) {
                regions = ordered_banding_regions(sample);
            }
            if (!regions.empty()) {
                for (const auto* region : regions) {
                    QualityEventThreshold threshold;
                    threshold.kind =
                        QualityEventThresholdKind::SpatialDetection;
                    threshold.value = region->detection_threshold;
                    candidates.push_back(Candidate{
                        &sample,
                        score,
                        region->score,
                        QualityEventClassification::SpatialCandidate,
                        threshold,
                        region,
                    });
                }
                continue;
            }

            constexpr double kMinimumRelativeExcess = 1e-9;
            if (relative_available &&
                score > relative_threshold.median + kMinimumRelativeExcess &&
                score + kMinimumRelativeExcess >= relative_threshold.value) {
                candidates.push_back(Candidate{
                    &sample,
                    score,
                    score,
                    QualityEventClassification::RelativeOutlier,
                    relative_threshold,
                    nullptr,
                });
            }
        }

        uint64_t current_sample_index =
            std::numeric_limits<uint64_t>::max();
        std::vector<size_t> previous_sample_events;
        std::vector<size_t> current_sample_events;
        std::vector<bool> previous_event_matched;
        for (const auto& candidate : candidates) {
            if (candidate.sample->sample_index != current_sample_index) {
                previous_sample_events =
                    std::move(current_sample_events);
                current_sample_events.clear();
                previous_event_matched.assign(
                    previous_sample_events.size(), false);
                current_sample_index = candidate.sample->sample_index;
            }

            size_t best_previous = std::numeric_limits<size_t>::max();
            double best_affinity = -1.0;
            for (size_t index = 0;
                 index < previous_sample_events.size();
                 ++index) {
                if (previous_event_matched[index]) {
                    continue;
                }
                const double affinity = merge_affinity(
                    events[previous_sample_events[index]],
                    candidate,
                    has_last_spatial_region[previous_sample_events[index]]
                        ? &last_spatial_regions[
                              previous_sample_events[index]]
                        : nullptr,
                    merge_gap_us);
                if (affinity > best_affinity) {
                    best_affinity = affinity;
                    best_previous = index;
                }
            }

            size_t event_index = 0;
            if (best_previous == std::numeric_limits<size_t>::max()) {
                events.push_back(event_from_candidate(metric.name, candidate));
                has_last_spatial_region.push_back(candidate.region != nullptr);
                last_spatial_regions.push_back(
                    candidate.region ? *candidate.region
                                     : QualitySpatialRegion{});
                event_index = events.size() - 1;
            } else {
                previous_event_matched[best_previous] = true;
                event_index = previous_sample_events[best_previous];
                merge_candidate(events[event_index], candidate);
                if (candidate.region) {
                    has_last_spatial_region[event_index] = true;
                    last_spatial_regions[event_index] = *candidate.region;
                }
            }
            current_sample_events.push_back(event_index);
        }
    }

    std::stable_sort(
        events.begin(), events.end(), [](const QualityEvent& left,
                                        const QualityEvent& right) {
            if (left.start_pts_us != right.start_pts_us) {
                return left.start_pts_us < right.start_pts_us;
            }
            if (left.peak_pts_us != right.peak_pts_us) {
                return left.peak_pts_us < right.peak_pts_us;
            }
            return left.metric < right.metric;
        });
    return events;
}

}  // namespace vr::analysis::quality
