#pragma once

#include "renderer/frame/frame_storage.h"

struct AVFrame;
struct AVCodecParameters;

namespace vr {

VideoColorInfo color_info_from_av_frame(const AVFrame* frame);
VideoColorInfo color_info_from_av_codec_parameters(const AVCodecParameters* params);

}  // namespace vr
