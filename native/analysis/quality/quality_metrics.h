#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vr::analysis::quality {

constexpr uint32_t kQualityReportSchemaVersion = 4;
constexpr const char* kQualityMetricVersion = "quality-demo-v5";
constexpr const char* kQualityBackendName = "cpu-reference";
constexpr uint32_t kQualityTileSchemaVersion = 1;
constexpr const char* kQualityTileSchemaId = "quality-tile-v1";
constexpr const char* kQualityTileMetricVersion = "quality-tile-metrics-v1";
constexpr int kQualityTileTargetSize = 64;

struct LumaPlaneView {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride_bytes = 0;
    int sample_step_bytes = 1;
    int sample_offset_bytes = 0;
    int bit_depth = 8;
    int sample_shift = 0;
};

enum class QualityCpuMode {
    Auto,
    Scalar,
};

struct DistributionSummary {
    uint64_t count = 0;
    double mean = 0.0;
    double p10 = 0.0;
    double p50 = 0.0;
    double p90 = 0.0;
    double p95 = 0.0;
    double maximum = 0.0;
};

/// An experimental defect region in the decoded luma coordinate space.
///
/// The frame-level proxy remains authoritative. Regions are supporting
/// evidence produced only by metrics that have a spatial implementation.
struct QualitySpatialRegion {
    std::string metric;
    double score = 0.0;
    double detection_threshold = 0.0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    uint32_t tile_count = 0;
    uint32_t tile_span_columns = 0;
    uint32_t tile_span_rows = 0;
    double fill_ratio = 0.0;
};

struct BandingMeasurement {
    double score = 0.0;
    std::vector<QualitySpatialRegion> regions;
};

struct FrameQualitySample {
    uint64_t sample_index = 0;
    uint64_t decoded_frame_index = 0;
    int64_t pts_us = 0;
    // Negative means that the metric was not requested. A valid metric score
    // is always in [0, 1].
    double blockiness = -1.0;
    double banding = -1.0;
    double blur = -1.0;
    double noise = -1.0;
    double flicker = -1.0;
    double average_qp = -1.0;
    std::vector<QualitySpatialRegion> spatial_regions;
    struct TileGrid {
        int target_tile_width = kQualityTileTargetSize;
        int target_tile_height = kQualityTileTargetSize;
        int columns = 0;
        int rows = 0;
        std::vector<double> blockiness;
        std::vector<double> banding;
        std::vector<double> blur;
        std::vector<double> noise;
        std::vector<double> flicker;
    } tile_grid;
};

struct QualityExecutionInfo {
    std::string requested_backend;
    std::string resolved_backend;
    std::string cpu_mode;
    std::string cpu_dispatch;
    uint32_t decode_threads_requested = 0;
    int decoder_thread_count = 0;
    int decoder_thread_type = 0;
    uint32_t cpu_workers = 0;
    uint32_t cpu_in_flight = 0;
    bool cpu_worker_pool_active = false;
    std::string gpu_adapter;
    uint32_t gpu_in_flight = 0;
    std::string scheduling;
};

struct LumaTemporalSignature {
    double mean_luma = 0.0;
    int columns = 0;
    int rows = 0;
    std::vector<double> tile_means;
};

enum class QualityTileMetric {
    Blockiness,
    Banding,
    Blur,
    Noise,
};

struct QualityTileMeasurement {
    int target_tile_width = kQualityTileTargetSize;
    int target_tile_height = kQualityTileTargetSize;
    int columns = 0;
    int rows = 0;
    // Row-major scores. Negative entries are explicitly unavailable because
    // a balanced edge tile is smaller than the metric's minimum input size.
    std::vector<double> scores;
};

struct StreamStatistics {
    uint64_t packet_count = 0;
    uint64_t packet_bytes = 0;
    uint64_t keyframe_packets = 0;
    uint64_t decoded_frames = 0;
    uint64_t i_frames = 0;
    uint64_t p_frames = 0;
    uint64_t b_frames = 0;
    DistributionSummary packet_size_bytes;
    DistributionSummary average_qp;
};

struct QualityMetricTimings {
    DistributionSummary blockiness_ms;
    DistributionSummary banding_ms;
    DistributionSummary blur_ms;
    DistributionSummary noise_ms;
    DistributionSummary temporal_ms;
    DistributionSummary gpu_pack_ms;
    DistributionSummary gpu_submit_ms;
    DistributionSummary gpu_wait_ms;
    DistributionSummary gpu_submit_wait_ms;
    DistributionSummary gpu_readback_ms;
    DistributionSummary gpu_total_ms;
    DistributionSummary gpu_latency_ms;
};

struct QualityReport {
    uint32_t schema_version = kQualityReportSchemaVersion;
    std::string metric_version = kQualityMetricVersion;
    std::string backend = kQualityBackendName;
    std::string backend_diagnostic;
    QualityExecutionInfo execution;
    int width = 0;
    int height = 0;
    int bit_depth = 0;
    int64_t sample_interval_us = 0;
    uint32_t max_samples = 0;
    bool truncated = false;
    uint64_t unsupported_pixel_frames = 0;
    StreamStatistics stream;
    DistributionSummary blockiness;
    DistributionSummary banding;
    DistributionSummary blur;
    DistributionSummary noise;
    DistributionSummary flicker;
    QualityMetricTimings timings;
    std::vector<FrameQualitySample> timeline;
};

bool is_valid_luma_plane(const LumaPlaneView& plane);
double measure_blockiness(const LumaPlaneView& plane);
double measure_blockiness(const LumaPlaneView& plane,
                          QualityCpuMode mode);
const char* quality_cpu_dispatch_name();
double measure_banding_proxy(const LumaPlaneView& plane);
double measure_banding_proxy(const LumaPlaneView& plane,
                             QualityCpuMode mode);
BandingMeasurement measure_banding_with_regions(
    const LumaPlaneView& plane);
BandingMeasurement measure_banding_with_regions(
    const LumaPlaneView& plane,
    QualityCpuMode mode);
BandingMeasurement measure_banding_with_regions(
    const LumaPlaneView& plane,
    QualityCpuMode mode,
    bool collect_spatial_regions);
double measure_blur_proxy(const LumaPlaneView& plane);
double measure_blur_proxy(const LumaPlaneView& plane,
                          QualityCpuMode mode);
double measure_noise_proxy(const LumaPlaneView& plane);
double measure_noise_proxy(const LumaPlaneView& plane,
                           QualityCpuMode mode);
QualityTileMeasurement measure_quality_tiles(
    const LumaPlaneView& plane,
    QualityTileMetric metric,
    QualityCpuMode mode = QualityCpuMode::Auto);
bool make_temporal_signature(const LumaPlaneView& plane,
                             LumaTemporalSignature& signature);
bool make_temporal_tile_signature(const LumaPlaneView& plane,
                                  LumaTemporalSignature& signature);
// Returns -1 when the signatures are invalid or a probable scene cut makes the
// three-frame estimate unreliable.
double measure_flicker_proxy(const LumaTemporalSignature& previous_previous,
                             const LumaTemporalSignature& previous,
                             const LumaTemporalSignature& current);
// Returns the frame proxy and row-major per-tile curvature from the same three
// signatures. On insufficient history, incompatible grids, or a probable scene
// cut, returns -1 and clears tile_scores.
double measure_flicker_proxy_with_tiles(
    const LumaTemporalSignature& previous_previous,
    const LumaTemporalSignature& previous,
    const LumaTemporalSignature& current,
    std::vector<double>& tile_scores);
DistributionSummary summarize_distribution(std::vector<double> values);

}  // namespace vr::analysis::quality
