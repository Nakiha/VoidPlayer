#pragma once

#include "analysis/quality/quality_metrics.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vr::analysis::quality {

struct WgpuQualityScores {
    double blockiness = 0.0;
    double banding = 0.0;
    double blur = 0.0;
    double noise = 0.0;
    double pack_ms = 0.0;
    double submit_ms = 0.0;
    double wait_ms = 0.0;
    double submit_wait_ms = 0.0;
    double readback_ms = 0.0;
    double total_ms = 0.0;
    double latency_ms = 0.0;
};

struct WgpuQualityCreationTimings {
    double instance_ms = 0.0;
    double adapter_ms = 0.0;
    double device_ms = 0.0;
    double pipeline_ms = 0.0;
    double total_ms = 0.0;
};

class WgpuQualityBackend {
public:
    using Ticket = uint64_t;

    enum class CollectStatus {
        Pending,
        Ready,
        Error,
    };

    static std::unique_ptr<WgpuQualityBackend> create(std::string& error);
    ~WgpuQualityBackend();

    WgpuQualityBackend(const WgpuQualityBackend&) = delete;
    WgpuQualityBackend& operator=(const WgpuQualityBackend&) = delete;

    const std::string& adapter_name() const { return adapter_name_; }
    const WgpuQualityCreationTimings& creation_timings() const {
        return creation_timings_;
    }
    uint32_t max_in_flight() const { return max_in_flight_; }
    bool submit_plane(const LumaPlaneView& plane,
                      Ticket& ticket,
                      std::string& error);
    CollectStatus try_collect_plane(Ticket ticket,
                                    WgpuQualityScores& scores,
                                    std::string& error);
    bool collect_plane(Ticket ticket,
                       WgpuQualityScores& scores,
                       std::string& error);
    bool score_plane(const LumaPlaneView& plane,
                     WgpuQualityScores& scores,
                     std::string& error);

private:
    explicit WgpuQualityBackend(
        void* context,
        std::string adapter_name,
        WgpuQualityCreationTimings creation_timings,
        uint32_t max_in_flight);

    void* context_ = nullptr;
    std::string adapter_name_;
    WgpuQualityCreationTimings creation_timings_;
    uint32_t max_in_flight_ = 0;
};

}  // namespace vr::analysis::quality
