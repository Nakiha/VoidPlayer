#pragma once

#include "media/seek_controller.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace vr {

constexpr int64_t kAudioNoPts = INT64_MIN;

struct PcmBufferStats {
    uint64_t serial = 0;
    size_t queued_frames = 0;
    int64_t queued_duration_us = 0;
    size_t underrun_frames = 0;
    size_t discarded_frames = 0;
    size_t seek_trimmed_frames = 0;
    size_t silence_frames_inserted = 0;
    size_t stale_chunks_dropped = 0;
    size_t seek_realign_count = 0;
    int64_t last_output_pts_us = kAudioNoPts;
    int64_t last_drift_us = 0;
    int64_t max_abs_drift_us = 0;
};

struct PcmPopResult {
    size_t requested_frames = 0;
    size_t pcm_frames = 0;
    size_t silence_frames = 0;
    int64_t first_pts_us = kAudioNoPts;
    uint64_t serial = 0;
};

class PcmBuffer {
public:
    PcmBuffer(int channels, int sample_rate, size_t capacity_frames);

    uint64_t current_serial() const;
    void begin_seek(int64_t target_pts_us, SeekType type);

    bool push(const int16_t* samples,
              size_t frames,
              int64_t pts_us,
              int64_t duration_us,
              uint64_t serial);
    PcmPopResult pop(int16_t* dst, size_t frames);
    void discard(size_t frames);
    void flush();
    void abort();

    PcmBufferStats stats() const;
    size_t queued_frames() const;

private:
    struct Chunk {
        std::vector<int16_t> samples;
        size_t frames = 0;
        int64_t pts_us = kAudioNoPts;
        int64_t duration_us = 0;
        uint64_t serial = 0;
    };

    int64_t frames_to_us(size_t frames) const;
    size_t us_to_frames(int64_t us) const;
    bool align_after_seek_locked(Chunk& chunk);
    void append_chunk_locked(Chunk chunk);
    void append_silence_locked(size_t frames, int64_t pts_us, uint64_t serial);
    void discard_locked(size_t frames);
    void update_drift_locked(int64_t first_pts_us, size_t pcm_frames);
    static int64_t abs_i64(int64_t value);

    const int channels_;
    const int sample_rate_;
    const size_t capacity_frames_;

    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<Chunk> chunks_;
    size_t queued_frames_ = 0;
    bool aborted_ = false;

    uint64_t serial_ = 0;
    bool seek_alignment_pending_ = false;
    int64_t seek_target_pts_us_ = 0;
    SeekType seek_type_ = SeekType::Keyframe;
    bool has_expected_output_pts_ = false;
    int64_t expected_output_pts_us_ = kAudioNoPts;
    PcmBufferStats stats_;
};

} // namespace vr
