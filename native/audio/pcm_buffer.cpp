#include "audio/pcm_buffer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace vr {

namespace {
constexpr int64_t kSmallSeekGapUs = 250000;
}

PcmBuffer::PcmBuffer(int channels, int sample_rate, size_t capacity_frames)
    : channels_(std::max(1, channels))
    , sample_rate_(std::max(1, sample_rate))
    , capacity_frames_(std::max<size_t>(1, capacity_frames)) {}

uint64_t PcmBuffer::current_serial() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serial_;
}

void PcmBuffer::begin_seek(int64_t target_pts_us, SeekType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++serial_;
    chunks_.clear();
    queued_frames_ = 0;
    seek_alignment_pending_ = true;
    seek_target_pts_us_ = target_pts_us;
    seek_type_ = type;
    has_expected_output_pts_ = false;
    expected_output_pts_us_ = kAudioNoPts;
    stats_.serial = serial_;
    stats_.queued_frames = 0;
    stats_.queued_duration_us = 0;
    not_full_.notify_all();
}

bool PcmBuffer::push(const int16_t* samples,
                     size_t frames,
                     int64_t pts_us,
                     int64_t duration_us,
                     uint64_t serial) {
    if (!samples || frames == 0) return false;

    Chunk chunk;
    chunk.frames = frames;
    chunk.pts_us = pts_us;
    chunk.duration_us = duration_us > 0 ? duration_us : frames_to_us(frames);
    chunk.serial = serial;
    chunk.samples.assign(samples, samples + frames * static_cast<size_t>(channels_));

    std::unique_lock<std::mutex> lock(mutex_);
    if (aborted_) return false;
    if (serial != serial_) {
        ++stats_.stale_chunks_dropped;
        return false;
    }
    if (!align_after_seek_locked(chunk)) {
        return false;
    }
    if (chunk.frames > capacity_frames_) {
        const size_t trim_frames = chunk.frames - capacity_frames_;
        const size_t trim_samples = trim_frames * static_cast<size_t>(channels_);
        chunk.samples.erase(chunk.samples.begin(),
                            chunk.samples.begin() + static_cast<std::ptrdiff_t>(trim_samples));
        chunk.frames = capacity_frames_;
        if (chunk.pts_us != kAudioNoPts) {
            chunk.pts_us += frames_to_us(trim_frames);
        }
        chunk.duration_us = frames_to_us(chunk.frames);
        stats_.discarded_frames += trim_frames;
    }
    while (!aborted_ && queued_frames_ + chunk.frames > capacity_frames_) {
        not_full_.wait_for(lock, std::chrono::milliseconds(5));
    }
    if (aborted_) return false;
    append_chunk_locked(std::move(chunk));
    not_empty_.notify_all();
    return true;
}

PcmPopResult PcmBuffer::pop(int16_t* dst, size_t frames) {
    PcmPopResult result;
    result.requested_frames = frames;
    if (!dst || frames == 0) return result;

    std::lock_guard<std::mutex> lock(mutex_);
    result.serial = serial_;
    size_t remaining = frames;
    size_t out_offset_frames = 0;
    while (remaining > 0) {
        if (chunks_.empty()) {
            const size_t sample_offset = out_offset_frames * static_cast<size_t>(channels_);
            const size_t samples_to_zero = remaining * static_cast<size_t>(channels_);
            std::memset(dst + sample_offset, 0, samples_to_zero * sizeof(int16_t));
            result.silence_frames += remaining;
            stats_.underrun_frames += remaining;
            remaining = 0;
            break;
        }

        Chunk& chunk = chunks_.front();
        if (result.first_pts_us == kAudioNoPts && chunk.pts_us != kAudioNoPts) {
            result.first_pts_us = chunk.pts_us;
        }
        const size_t take = std::min(remaining, chunk.frames);
        const size_t sample_count = take * static_cast<size_t>(channels_);
        const size_t sample_offset = out_offset_frames * static_cast<size_t>(channels_);
        std::copy_n(chunk.samples.data(), sample_count, dst + sample_offset);

        result.pcm_frames += take;
        remaining -= take;
        out_offset_frames += take;
        queued_frames_ -= take;

        if (take == chunk.frames) {
            chunks_.pop_front();
        } else {
            chunk.samples.erase(chunk.samples.begin(),
                                chunk.samples.begin() + static_cast<std::ptrdiff_t>(sample_count));
            chunk.frames -= take;
            if (chunk.pts_us != kAudioNoPts) {
                chunk.pts_us += frames_to_us(take);
            }
            chunk.duration_us = frames_to_us(chunk.frames);
        }
    }
    update_drift_locked(result.first_pts_us, result.pcm_frames);
    stats_.queued_frames = queued_frames_;
    stats_.queued_duration_us = frames_to_us(queued_frames_);
    not_full_.notify_all();
    return result;
}

void PcmBuffer::discard(size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    discard_locked(frames);
    not_full_.notify_all();
}

void PcmBuffer::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
    queued_frames_ = 0;
    has_expected_output_pts_ = false;
    expected_output_pts_us_ = kAudioNoPts;
    stats_.queued_frames = 0;
    stats_.queued_duration_us = 0;
    not_full_.notify_all();
}

void PcmBuffer::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    chunks_.clear();
    queued_frames_ = 0;
    not_full_.notify_all();
    not_empty_.notify_all();
}

PcmBufferStats PcmBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PcmBufferStats snapshot = stats_;
    snapshot.serial = serial_;
    snapshot.queued_frames = queued_frames_;
    snapshot.queued_duration_us = frames_to_us(queued_frames_);
    return snapshot;
}

size_t PcmBuffer::queued_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_frames_;
}

int64_t PcmBuffer::frames_to_us(size_t frames) const {
    return static_cast<int64_t>(
        (static_cast<int64_t>(frames) * 1000000LL) / static_cast<int64_t>(sample_rate_));
}

size_t PcmBuffer::us_to_frames(int64_t us) const {
    if (us <= 0) return 0;
    return static_cast<size_t>(
        (us * static_cast<int64_t>(sample_rate_) + 999999LL) / 1000000LL);
}

bool PcmBuffer::align_after_seek_locked(Chunk& chunk) {
    if (!seek_alignment_pending_) return true;
    if (chunk.pts_us == kAudioNoPts || chunk.duration_us <= 0) {
        seek_alignment_pending_ = false;
        ++stats_.seek_realign_count;
        return true;
    }

    const int64_t chunk_end_us = chunk.pts_us + chunk.duration_us;
    if (chunk_end_us <= seek_target_pts_us_) {
        stats_.discarded_frames += chunk.frames;
        stats_.seek_trimmed_frames += chunk.frames;
        return false;
    }

    if (chunk.pts_us < seek_target_pts_us_) {
        const int64_t trim_us = seek_target_pts_us_ - chunk.pts_us;
        const size_t trim_frames = std::min(chunk.frames, us_to_frames(trim_us));
        const size_t trim_samples = trim_frames * static_cast<size_t>(channels_);
        chunk.samples.erase(chunk.samples.begin(),
                            chunk.samples.begin() + static_cast<std::ptrdiff_t>(trim_samples));
        chunk.frames -= trim_frames;
        chunk.pts_us += frames_to_us(trim_frames);
        chunk.duration_us = frames_to_us(chunk.frames);
        stats_.discarded_frames += trim_frames;
        stats_.seek_trimmed_frames += trim_frames;
    } else if (seek_type_ == SeekType::Exact) {
        const int64_t gap_us = chunk.pts_us - seek_target_pts_us_;
        if (gap_us > 0 && gap_us <= kSmallSeekGapUs) {
            const size_t silence_frames = us_to_frames(gap_us);
            append_silence_locked(silence_frames, seek_target_pts_us_, chunk.serial);
        } else if (gap_us > 0) {
            ++stats_.seek_realign_count;
        }
    } else if (chunk.pts_us != seek_target_pts_us_) {
        ++stats_.seek_realign_count;
    }

    seek_alignment_pending_ = false;
    return chunk.frames > 0;
}

void PcmBuffer::append_chunk_locked(Chunk chunk) {
    queued_frames_ += chunk.frames;
    stats_.queued_frames = queued_frames_;
    stats_.queued_duration_us = frames_to_us(queued_frames_);
    chunks_.push_back(std::move(chunk));
}

void PcmBuffer::append_silence_locked(size_t frames, int64_t pts_us, uint64_t serial) {
    if (frames == 0) return;
    Chunk silence;
    silence.frames = frames;
    silence.pts_us = pts_us;
    silence.duration_us = frames_to_us(frames);
    silence.serial = serial;
    silence.samples.assign(frames * static_cast<size_t>(channels_), 0);
    stats_.silence_frames_inserted += frames;
    append_chunk_locked(std::move(silence));
}

void PcmBuffer::discard_locked(size_t frames) {
    size_t remaining = frames;
    while (remaining > 0 && !chunks_.empty()) {
        Chunk& chunk = chunks_.front();
        const size_t take = std::min(remaining, chunk.frames);
        const size_t sample_count = take * static_cast<size_t>(channels_);
        if (take == chunk.frames) {
            chunks_.pop_front();
        } else {
            chunk.samples.erase(chunk.samples.begin(),
                                chunk.samples.begin() + static_cast<std::ptrdiff_t>(sample_count));
            chunk.frames -= take;
            if (chunk.pts_us != kAudioNoPts) {
                chunk.pts_us += frames_to_us(take);
            }
            chunk.duration_us = frames_to_us(chunk.frames);
        }
        queued_frames_ -= take;
        stats_.discarded_frames += take;
        remaining -= take;
    }
    stats_.queued_frames = queued_frames_;
    stats_.queued_duration_us = frames_to_us(queued_frames_);
}

void PcmBuffer::update_drift_locked(int64_t first_pts_us, size_t pcm_frames) {
    if (first_pts_us != kAudioNoPts && has_expected_output_pts_) {
        const int64_t drift = first_pts_us - expected_output_pts_us_;
        stats_.last_drift_us = drift;
        stats_.max_abs_drift_us = std::max(stats_.max_abs_drift_us, abs_i64(drift));
    }
    if (first_pts_us != kAudioNoPts) {
        stats_.last_output_pts_us = first_pts_us;
        expected_output_pts_us_ = first_pts_us + frames_to_us(pcm_frames);
        has_expected_output_pts_ = true;
    } else if (has_expected_output_pts_) {
        expected_output_pts_us_ += frames_to_us(pcm_frames);
    }
}

int64_t PcmBuffer::abs_i64(int64_t value) {
    return value < 0 ? -value : value;
}

} // namespace vr
