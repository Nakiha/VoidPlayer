#pragma once

#include "renderer/buffer/bidi_ring_buffer.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

struct AVFrame;

namespace vr {

struct SoftwareFramePackTiming {
    uint64_t layout_us = 0;
    uint64_t alloc_us = 0;
    uint64_t pack_us = 0;
    uint64_t commit_us = 0;
};

bool supported_software_format(AVPixelFormat format);
bool software_format_uses_p010(AVPixelFormat format);
bool software_format_uses_direct_planar_yuv420(AVPixelFormat format);
bool software_format_uses_direct_yuv420_storage(AVPixelFormat format);
bool validate_software_frame_layout(int width, int height, AVPixelFormat format);
bool wrap_frame_as_cpu_planar_yuv420(const AVFrame* frame, TextureFrame& result);
bool wrap_frame_as_cpu_yuv420_storage(const AVFrame* frame, TextureFrame& result);
bool convert_frame_to_cpu_nv12(const AVFrame* frame,
                               const char* context,
                               TextureFrame& result,
                               SoftwareFramePackTiming* timing = nullptr);

} // namespace vr
