#include "renderer/decode/frame_identity.h"

#include "media/source_packet_identity.h"

namespace vr {

void populate_frame_identity_from_av_frame(const AVFrame* frame, TextureFrame& out) {
    SourcePacketIdentity identity;
    if (!source_packet_identity_from_frame(frame, identity)) {
        return;
    }
    out.source_packet_index = identity.packet_index;
    out.source_packet_size = identity.size;
    out.source_packet_pos = identity.position;
    out.source_packet_pts = identity.pts;
    out.source_packet_dts = identity.dts;
    out.frame_identity_mode = FrameIdentityMode::SourcePacketIdentity;
}

const char* frame_identity_mode_name(FrameIdentityMode mode) {
    switch (mode) {
    case FrameIdentityMode::RuntimeOrdinal:
        return "runtimeOrdinal";
    case FrameIdentityMode::TimestampEstimated:
        return "timestampEstimated";
    case FrameIdentityMode::SourcePacketIdentity:
        return "sourcePacketIdentity";
    case FrameIdentityMode::ExactAnalysisFrame:
        return "exactAnalysisFrame";
    case FrameIdentityMode::Unknown:
    default:
        return "unknown";
    }
}

} // namespace vr
