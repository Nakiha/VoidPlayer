#pragma once

#include "video_renderer/track/track_pipeline.h"

namespace vr {

DecodeDeviceMode default_decode_device_mode(AVCodecID codec_id);

class TrackPipelineFactory {
public:
    /// Build a pipeline with demux opened and decode started, but leave the
    /// demux worker stopped so owners can wire seek/error/audio callbacks first.
    std::unique_ptr<TrackPipeline> create_opened_pipeline(
        const std::string& path,
        bool hw_decode,
        const SeekRequest* initial_seek = nullptr) const;
};

} // namespace vr
