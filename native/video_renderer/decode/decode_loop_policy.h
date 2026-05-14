#pragma once

#include "video_renderer/buffer/track_buffer.h"

#include <cstddef>

namespace vr {

bool should_publish_pending_exact_seek_frames(size_t pending_count,
                                              bool decode_paused,
                                              TrackState output_state);

bool should_drain_decoder_before_next_packet(bool drain_requested,
                                             bool decode_paused,
                                             TrackState output_state);

bool should_pause_decode_consumption(bool decode_paused,
                                     TrackState output_state);

bool should_discard_packet_before_decode(bool seek_pending,
                                         bool decode_paused,
                                         TrackState output_state);

} // namespace vr
