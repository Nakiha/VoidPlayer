#pragma once

#include "analysis/quality/quality_metrics.h"

#include <cstdint>
#include <string>

namespace vr::analysis::quality {

enum class QualityComputeBackend {
    Cpu,
    Wgpu,
    Auto,
};

struct QualityVideoAnalyzerOptions {
    int64_t sample_interval_us = 1'000'000;
    uint32_t max_samples = 0;
    QualityComputeBackend backend = QualityComputeBackend::Cpu;
    QualityCpuMode cpu_mode = QualityCpuMode::Auto;
    uint32_t decode_threads = 0;
    uint32_t cpu_workers = 0;
    uint32_t cpu_in_flight = 0;
};

bool analyze_video_quality(const std::string& video_path,
                           const QualityVideoAnalyzerOptions& options,
                           QualityReport& report,
                           std::string& error);

}  // namespace vr::analysis::quality
