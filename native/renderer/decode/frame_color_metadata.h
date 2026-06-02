#pragma once

#include "renderer/frame/frame_storage.h"

struct AVFrame;

namespace vr {

VideoColorInfo color_info_from_av_frame(const AVFrame* frame);

}  // namespace vr
