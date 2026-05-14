#pragma once

#include <cstdint>
#include <string>

namespace vr::tools {

struct AnalysisOverlayGpuBenchmarkOptions {
    std::string path;
    std::string mode = "bitrate";
    uint32_t frame = UINT32_MAX;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t iterations = 120;
    bool with_grid = false;
    bool json = false;
};

int benchmark_analysis_overlay_gpu(const AnalysisOverlayGpuBenchmarkOptions& options);

} // namespace vr::tools
