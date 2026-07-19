#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vr::analysis::quality {

constexpr uint32_t kQualityReportSchemaVersion = 4;
constexpr const char* kQualityMetricVersion = "quality-demo-v3";
constexpr const char* kQualityBackendName = "cpu-reference";

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

struct FrameQualitySample {
    uint64_t sample_index = 0;
    uint64_t decoded_frame_index = 0;
    int64_t pts_us = 0;
    double blockiness = 0.0;
    double banding = 0.0;
    double blur = 0.0;
    double noise = 0.0;
    double flicker = -1.0;
    double average_qp = -1.0;
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
    std::vector<double> tile_means;
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
double measure_blur_proxy(const LumaPlaneView& plane);
double measure_blur_proxy(const LumaPlaneView& plane,
                          QualityCpuMode mode);
double measure_noise_proxy(const LumaPlaneView& plane);
double measure_noise_proxy(const LumaPlaneView& plane,
                           QualityCpuMode mode);
bool make_temporal_signature(const LumaPlaneView& plane,
                             LumaTemporalSignature& signature);
// Returns -1 when the signatures are invalid or a probable scene cut makes the
// three-frame estimate unreliable.
double measure_flicker_proxy(const LumaTemporalSignature& previous_previous,
                             const LumaTemporalSignature& previous,
                             const LumaTemporalSignature& current);
DistributionSummary summarize_distribution(std::vector<double> values);

}  // namespace vr::analysis::quality
