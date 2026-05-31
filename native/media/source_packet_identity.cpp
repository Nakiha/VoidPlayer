#include "media/source_packet_identity.h"

#include <cstring>

extern "C" {
#include <libavutil/buffer.h>
}

namespace vr {

namespace {

bool valid_identity(const SourcePacketIdentity& identity) {
    return identity.magic == kSourcePacketIdentityMagic &&
           identity.version == kSourcePacketIdentityVersion;
}

} // namespace

bool attach_source_packet_identity(AVPacket* packet, const SourcePacketIdentity& identity) {
    if (!packet || !valid_identity(identity)) {
        return false;
    }
    AVBufferRef* buffer = av_buffer_alloc(sizeof(SourcePacketIdentity));
    if (!buffer || !buffer->data) {
        av_buffer_unref(&buffer);
        return false;
    }
    std::memcpy(buffer->data, &identity, sizeof(SourcePacketIdentity));
    av_buffer_unref(&packet->opaque_ref);
    packet->opaque_ref = buffer;
    packet->opaque = nullptr;
    return true;
}

bool source_packet_identity_from_frame(const AVFrame* frame, SourcePacketIdentity& out) {
    out = {};
    if (!frame || !frame->opaque_ref ||
        frame->opaque_ref->size < static_cast<int>(sizeof(SourcePacketIdentity)) ||
        !frame->opaque_ref->data) {
        return false;
    }
    SourcePacketIdentity identity{};
    std::memcpy(&identity, frame->opaque_ref->data, sizeof(SourcePacketIdentity));
    if (!valid_identity(identity)) {
        return false;
    }
    out = identity;
    return true;
}

} // namespace vr
