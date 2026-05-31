#pragma once

#include <cstdint>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
}

namespace vr {

constexpr uint32_t kSourcePacketIdentityMagic = 0x56504944u; // "VPID"
constexpr uint32_t kSourcePacketIdentityVersion = 1;

struct SourcePacketIdentity {
    uint32_t magic = kSourcePacketIdentityMagic;
    uint32_t version = kSourcePacketIdentityVersion;
    int32_t stream_index = -1;
    int32_t packet_index = -1;
    int64_t position = -1;
    int64_t pts = INT64_MIN;
    int64_t dts = INT64_MIN;
    int64_t duration = 0;
    int32_t size = 0;
    int32_t flags = 0;
};

bool attach_source_packet_identity(AVPacket* packet, const SourcePacketIdentity& identity);
bool source_packet_identity_from_frame(const AVFrame* frame, SourcePacketIdentity& out);

} // namespace vr
