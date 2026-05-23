#pragma once

#include "video_renderer/frame/frame_storage.h"

#include <cstdint>

namespace vr {

int default_yuv_color_matrix_for_size(int width, int height);

void write_yuv_to_bgra(uint8_t y,
                       uint8_t u,
                       uint8_t v,
                       int color_range,
                       int color_matrix,
                       uint8_t* out);

}  // namespace vr
