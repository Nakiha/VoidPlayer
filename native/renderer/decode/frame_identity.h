#pragma once

#include "renderer/buffer/bidi_ring_buffer.h"

struct AVFrame;

namespace vr {

void populate_frame_identity_from_av_frame(const AVFrame* frame, TextureFrame& out);

const char* frame_identity_mode_name(FrameIdentityMode mode);

} // namespace vr
