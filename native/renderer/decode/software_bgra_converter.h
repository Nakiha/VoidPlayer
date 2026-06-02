#pragma once

#include <cstddef>
#include <cstdint>

struct AVFrame;

namespace vr {

bool convert_software_frame_to_bgra(const AVFrame* frame, uint8_t* dst, size_t dst_size);

}  // namespace vr
