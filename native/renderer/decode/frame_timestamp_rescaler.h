#pragma once

extern "C" {
#include <libavutil/rational.h>
}

struct AVFrame;

namespace vr {

void rescale_frame_timestamps_to_us(AVFrame* frame, AVRational time_base);

} // namespace vr
