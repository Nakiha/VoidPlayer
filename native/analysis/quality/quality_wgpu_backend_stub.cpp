#include "analysis/quality/quality_wgpu_backend.h"

#include <utility>

namespace vr::analysis::quality {

WgpuQualityBackend::WgpuQualityBackend(
    void* context,
    std::string adapter_name,
    WgpuQualityCreationTimings creation_timings,
    uint32_t max_in_flight)
    : context_(context),
      adapter_name_(std::move(adapter_name)),
      creation_timings_(creation_timings),
      max_in_flight_(max_in_flight) {}

WgpuQualityBackend::~WgpuQualityBackend() = default;

std::unique_ptr<WgpuQualityBackend> WgpuQualityBackend::create(
    std::string& error) {
    error =
        "wgpu quality backend was disabled when VoidPlayerCli was built";
    return nullptr;
}

bool WgpuQualityBackend::submit_plane(const LumaPlaneView&,
                                      Ticket& ticket,
                                      std::string& error) {
    ticket = 0;
    error = "wgpu quality backend is not available in this build";
    return false;
}

WgpuQualityBackend::CollectStatus WgpuQualityBackend::try_collect_plane(
    Ticket,
    WgpuQualityScores& scores,
    std::string& error) {
    scores = WgpuQualityScores{};
    error = "wgpu quality backend is not available in this build";
    return CollectStatus::Error;
}

bool WgpuQualityBackend::collect_plane(Ticket,
                                       WgpuQualityScores& scores,
                                       std::string& error) {
    scores = WgpuQualityScores{};
    error = "wgpu quality backend is not available in this build";
    return false;
}

bool WgpuQualityBackend::score_plane(const LumaPlaneView&,
                                     WgpuQualityScores& scores,
                                     std::string& error) {
    scores = WgpuQualityScores{};
    error = "wgpu quality backend is not available in this build";
    return false;
}

}  // namespace vr::analysis::quality
