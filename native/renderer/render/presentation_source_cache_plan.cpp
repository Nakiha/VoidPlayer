#include "renderer/render/presentation_source_cache_plan.h"

#include <algorithm>

namespace vr {
namespace {

std::string source_slots_signature(const std::array<bool, 4>& requested_slots) {
    std::string signature = "slots=";
    for (size_t i = 0; i < requested_slots.size(); ++i) {
        if (requested_slots[i]) {
            signature += std::to_string(i) + ",";
        }
    }
    return signature;
}

} // namespace

PresentationSourceCachePlan build_presentation_source_cache_plan(
    const PresentationSourceCacheRequest& request,
    const std::vector<TrackInfo>& tracks) {
    PresentationSourceCachePlan plan;
    int requested_count = 0;
    for (const bool requested : request.requested_slots) {
        requested_count += requested ? 1 : 0;
    }
    if (requested_count <= 0 || requested_count > 4) {
        plan.error = "source-cache-empty-request";
        plan.signature = source_slots_signature(request.requested_slots);
        return plan;
    }

    for (const auto& info : tracks) {
        if (info.slot < 0 || info.slot >= 4 ||
            !request.requested_slots[static_cast<size_t>(info.slot)]) {
            continue;
        }
        if (info.file_id < 0 || info.width <= 0 || info.height <= 0) {
            plan.error = "source-cache-invalid-track";
            plan.signature = source_slots_signature(request.requested_slots) +
                "|tracks=" + std::to_string(tracks.size());
            return plan;
        }
        plan.tracks.push_back(PresentationSourceCacheTrackDescriptor{
            info.slot,
            info.file_id,
            info.width,
            info.height,
            info.color.transfer,
        });
    }

    std::sort(
        plan.tracks.begin(),
        plan.tracks.end(),
        [](const auto& left, const auto& right) {
            return left.slot < right.slot;
        });

    if (static_cast<int>(plan.tracks.size()) != requested_count) {
        plan.error = "source-cache-track-mismatch";
        plan.signature = source_slots_signature(request.requested_slots) +
            "|tracks=" + std::to_string(tracks.size());
        return plan;
    }

    plan.complete = true;
    plan.error = "none";
    plan.signature = "R16G16B16A16_FLOAT|";
    for (const int slot : request.source_order) {
        plan.signature += std::to_string(slot) + ",";
    }
    for (const auto& track : plan.tracks) {
        plan.signature += "|" + std::to_string(track.slot) + ":" +
            std::to_string(track.file_id) + ":" +
            std::to_string(track.width) + "x" +
            std::to_string(track.height) + ":" +
            std::to_string(track.color_transfer);
    }
    return plan;
}

} // namespace vr
