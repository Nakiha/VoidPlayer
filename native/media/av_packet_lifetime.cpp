#include "media/av_packet_lifetime.h"

extern "C" {
#include <libavcodec/packet.h>
}

namespace vr {

AvPacketOwner::AvPacketOwner(AVPacket* packet) noexcept
    : packet_(packet) {}

AvPacketOwner::~AvPacketOwner() {
    reset();
}

AvPacketOwner::AvPacketOwner(AvPacketOwner&& other) noexcept
    : packet_(other.release()) {}

AvPacketOwner& AvPacketOwner::operator=(AvPacketOwner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

AvPacketOwner AvPacketOwner::allocate() noexcept {
    return AvPacketOwner(av_packet_alloc());
}

AvPacketOwner AvPacketOwner::clone(const AVPacket* packet) noexcept {
    return AvPacketOwner(packet ? av_packet_clone(packet) : nullptr);
}

AVPacket* AvPacketOwner::get() const noexcept {
    return packet_;
}

AVPacket* AvPacketOwner::release() noexcept {
    AVPacket* packet = packet_;
    packet_ = nullptr;
    return packet;
}

void AvPacketOwner::reset(AVPacket* packet) noexcept {
    if (packet_) {
        av_packet_free(&packet_);
    }
    packet_ = packet;
}

AvPacketOwner::operator bool() const noexcept {
    return packet_ != nullptr;
}

} // namespace vr
