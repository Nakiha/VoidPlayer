#pragma once

#include <cstdint>
#include <optional>

extern "C" {
#include <libavutil/rational.h>
}

struct AVFrame;

namespace vr {

struct FrameTimestampNormalizationResult {
    int64_t raw_pts_us = 0;
    int64_t best_effort_pts_us = 0;
    int64_t output_pts_us = 0;
    bool raw_pts_available = false;
    bool best_effort_available = false;
    bool used_best_effort = false;
    bool adjusted_for_monotonicity = false;
    uint64_t adjustment_count = 0;
};

// Preserves AVCodec's output order and frame count while assigning each frame
// a strictly increasing presentation timestamp. FFmpeg has already performed
// codec-level picture reordering before avcodec_receive_frame() returns; this
// class never moves or drops frames.
class FrameTimestampNormalizer {
public:
    explicit FrameTimestampNormalizer(AVRational time_base);

    FrameTimestampNormalizationResult normalize(AVFrame* frame);
    void reset();

private:
    int64_t correction_step_us(int64_t duration_us) const;
    void observe_forward_delta(int64_t delta_us);

    AVRational time_base_;
    std::optional<int64_t> last_output_pts_us_;
    std::optional<int64_t> observed_cadence_us_;
    uint64_t adjustment_count_ = 0;
};

void rescale_frame_timestamps_to_us(AVFrame* frame, AVRational time_base);

} // namespace vr
