#include "media/source_packet_identity.h"

#include "media/ffmpeg_lifetime.h"

#include <cstring>

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
    auto buffer = AvBufferRefOwner::allocate(sizeof(SourcePacketIdentity));
    if (!buffer || !buffer.get()->data) {
        return false;
    }
    std::memcpy(buffer.get()->data, &identity, sizeof(SourcePacketIdentity));
    av_buffer_unref(&packet->opaque_ref);
    packet->opaque_ref = buffer.release();
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
