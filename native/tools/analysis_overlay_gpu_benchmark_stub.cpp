#include "analysis_overlay_gpu_benchmark.h"

#include <iostream>

namespace vr::tools {

int benchmark_analysis_overlay_gpu(const AnalysisOverlayGpuBenchmarkOptions& options) {
    if (options.json) {
        std::cout
            << "{"
            << "\"type\":\"overlayGpuBenchmark\","
            << "\"ok\":false,"
            << "\"deviceType\":\"unsupported\","
            << "\"error\":\"analysis overlay GPU benchmark has no active GPU backend\""
            << "}\n";
    } else {
        std::cerr << "analysis overlay GPU benchmark has no active GPU backend\n";
    }
    return 2;
}

} // namespace vr::tools
