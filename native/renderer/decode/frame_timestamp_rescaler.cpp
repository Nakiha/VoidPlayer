#include "renderer/decode/frame_timestamp_rescaler.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
}

namespace vr {

void rescale_frame_timestamps_to_us(AVFrame* frame, AVRational time_base) {
    if (!frame) {
        return;
    }
    if (frame->pts != AV_NOPTS_VALUE) {
        frame->pts = av_rescale_q(frame->pts, time_base, {1, 1000000});
    } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        frame->pts = av_rescale_q(frame->best_effort_timestamp, time_base, {1, 1000000});
    }
    if (frame->pkt_dts != AV_NOPTS_VALUE) {
        frame->pkt_dts = av_rescale_q(frame->pkt_dts, time_base, {1, 1000000});
    }
    if (frame->duration > 0 && frame->duration != AV_NOPTS_VALUE) {
        frame->duration = av_rescale_q(frame->duration, time_base, {1, 1000000});
    }
}

} // namespace vr
