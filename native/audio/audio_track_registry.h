#pragma once

#include "media/seek_controller.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace vr {

class PcmBuffer;

class AudioTrackController {
public:
    virtual ~AudioTrackController() = default;

    virtual void stop() = 0;
    virtual void set_paused(bool paused) = 0;
    virtual void notify_seek(int64_t target_pts_us, SeekType type) = 0;
};

struct AudioTrackHandle {
    int file_id = 0;
    std::shared_ptr<PcmBuffer> buffer;
    std::unique_ptr<AudioTrackController> decoder;

    AudioTrackHandle() = default;
    AudioTrackHandle(int file_id,
                     std::shared_ptr<PcmBuffer> buffer,
                     std::unique_ptr<AudioTrackController> decoder);
    AudioTrackHandle(AudioTrackHandle&&) noexcept = default;
    AudioTrackHandle& operator=(AudioTrackHandle&&) noexcept = default;
    AudioTrackHandle(const AudioTrackHandle&) = delete;
    AudioTrackHandle& operator=(const AudioTrackHandle&) = delete;
};

class AudioTrackRegistry {
public:
    std::optional<AudioTrackHandle> add_or_replace(
        int file_id,
        std::shared_ptr<PcmBuffer> buffer,
        std::unique_ptr<AudioTrackController> decoder);

    std::optional<AudioTrackHandle> remove(int file_id);
    std::vector<AudioTrackHandle> clear();

    bool set_track_decode_paused(int file_id, bool paused);
    void set_all_decode_paused(bool paused);
    bool notify_seek(int file_id, int64_t target_pts_us, SeekType type);

    std::map<int, std::shared_ptr<PcmBuffer>> buffers() const;
    size_t size() const;
    bool empty() const;

private:
    std::map<int, AudioTrackHandle> tracks_;
};

} // namespace vr
