#pragma once

struct AVPacket;

namespace vr {

class AvPacketOwner {
public:
    AvPacketOwner() noexcept = default;
    explicit AvPacketOwner(AVPacket* packet) noexcept;
    ~AvPacketOwner();

    AvPacketOwner(const AvPacketOwner&) = delete;
    AvPacketOwner& operator=(const AvPacketOwner&) = delete;

    AvPacketOwner(AvPacketOwner&& other) noexcept;
    AvPacketOwner& operator=(AvPacketOwner&& other) noexcept;

    static AvPacketOwner allocate() noexcept;
    static AvPacketOwner clone(const AVPacket* packet) noexcept;
    AVPacket* get() const noexcept;
    AVPacket* release() noexcept;
    void reset(AVPacket* packet = nullptr) noexcept;
    explicit operator bool() const noexcept;

private:
    AVPacket* packet_ = nullptr;
};

} // namespace vr
