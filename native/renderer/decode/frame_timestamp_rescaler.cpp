#include "renderer/decode/frame_timestamp_rescaler.h"

#include <limits>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
}

namespace vr {
namespace {

constexpr int64_t kDefaultFrameCadenceUs = 33333;
constexpr int64_t kMinObservedCadenceUs = 1000;
constexpr int64_t kMaxObservedCadenceUs = 1000000;

std::optional<int64_t> timestamp_us(int64_t timestamp, AVRational time_base) {
    if (timestamp == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    return av_rescale_q(timestamp, time_base, {1, 1000000});
}

int64_t add_saturated(int64_t value, int64_t delta) {
    if (delta > 0 && value > std::numeric_limits<int64_t>::max() - delta) {
        return std::numeric_limits<int64_t>::max();
    }
    return value + delta;
}

} // namespace

FrameTimestampNormalizer::FrameTimestampNormalizer(AVRational time_base)
    : time_base_(time_base) {}

void FrameTimestampNormalizer::reset() {
    last_output_pts_us_.reset();
    observed_cadence_us_.reset();
    adjustment_count_ = 0;
}

int64_t FrameTimestampNormalizer::correction_step_us(int64_t duration_us) const {
    const bool duration_is_sane =
        duration_us >= kMinObservedCadenceUs &&
        duration_us <= kMaxObservedCadenceUs;
    if (!observed_cadence_us_.has_value()) {
        return duration_is_sane ? duration_us : kDefaultFrameCadenceUs;
    }
    if (!duration_is_sane) {
        return *observed_cadence_us_;
    }

    // Reject wildly inconsistent duration metadata for correction purposes.
    // The normal forward path still preserves genuine VFR timestamps.
    const int64_t cadence = *observed_cadence_us_;
    if (duration_us < cadence / 4 || duration_us > cadence * 4) {
        return cadence;
    }
    return duration_us;
}

void FrameTimestampNormalizer::observe_forward_delta(int64_t delta_us) {
    if (delta_us < kMinObservedCadenceUs ||
        delta_us > kMaxObservedCadenceUs) {
        return;
    }
    if (!observed_cadence_us_.has_value() ||
        delta_us < *observed_cadence_us_) {
        observed_cadence_us_ = delta_us;
    }
}

FrameTimestampNormalizationResult FrameTimestampNormalizer::normalize(
    AVFrame* frame) {
    FrameTimestampNormalizationResult result;
    if (!frame) {
        return result;
    }

    const auto raw_pts_us = timestamp_us(frame->pts, time_base_);
    const auto best_effort_pts_us =
        timestamp_us(frame->best_effort_timestamp, time_base_);
    result.raw_pts_available = raw_pts_us.has_value();
    result.best_effort_available = best_effort_pts_us.has_value();
    result.raw_pts_us = raw_pts_us.value_or(0);
    result.best_effort_pts_us = best_effort_pts_us.value_or(0);

    if (frame->pkt_dts != AV_NOPTS_VALUE) {
        frame->pkt_dts = av_rescale_q(
            frame->pkt_dts, time_base_, {1, 1000000});
    }
    if (frame->duration > 0 && frame->duration != AV_NOPTS_VALUE) {
        frame->duration = av_rescale_q(
            frame->duration, time_base_, {1, 1000000});
    }

    // best_effort_timestamp is FFmpeg's presentation-time estimate after the
    // decoder has emitted the frame. Prefer it even when raw AVFrame.pts is
    // present; raw PTS can still follow packet/coded order for B-frame media.
    int64_t candidate_pts_us = 0;
    if (best_effort_pts_us.has_value()) {
        candidate_pts_us = *best_effort_pts_us;
        result.used_best_effort = true;
    } else if (raw_pts_us.has_value()) {
        candidate_pts_us = *raw_pts_us;
    } else if (last_output_pts_us_.has_value()) {
        candidate_pts_us = add_saturated(
            *last_output_pts_us_, correction_step_us(frame->duration));
        result.adjusted_for_monotonicity = true;
    } else {
        result.adjusted_for_monotonicity = true;
    }

    if (last_output_pts_us_.has_value()) {
        if (candidate_pts_us <= *last_output_pts_us_) {
            candidate_pts_us = add_saturated(
                *last_output_pts_us_, correction_step_us(frame->duration));
            result.adjusted_for_monotonicity = true;
        } else {
            observe_forward_delta(candidate_pts_us - *last_output_pts_us_);
        }
    }

    if (result.adjusted_for_monotonicity) {
        ++adjustment_count_;
    }
    result.adjustment_count = adjustment_count_;
    result.output_pts_us = candidate_pts_us;
    frame->pts = candidate_pts_us;
    last_output_pts_us_ = candidate_pts_us;
    return result;
}

void rescale_frame_timestamps_to_us(AVFrame* frame, AVRational time_base) {
    FrameTimestampNormalizer normalizer(time_base);
    normalizer.normalize(frame);
}

} // namespace vr
