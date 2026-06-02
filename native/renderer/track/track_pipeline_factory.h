#pragma once

#include "renderer/track/track_pipeline.h"
#include "renderer/render/backend_type.h"

#include <cstddef>
#include <mutex>

namespace vr {

DecodeDeviceMode default_decode_device_mode(AVCodecID codec_id);

struct TrackPipelineOpenOptions {
    RenderBackendKind render_backend = RenderBackendKind::D3D11;
    bool use_default_decode_device_mode = true;
    DecodeDeviceMode decode_device_mode = DecodeDeviceMode::IndependentDevice;
    void* render_device = nullptr;
    std::recursive_mutex* device_mutex = nullptr;
    size_t packet_queue_capacity = 0;
};

class TrackPipelineFactory {
public:
    /// Build a pipeline with demux opened and decode started, but leave the
    /// demux worker stopped so owners can wire seek/error/audio callbacks first.
    std::unique_ptr<TrackPipeline> create_opened_pipeline(
        const std::string& path,
        bool hw_decode,
        const SeekRequest* initial_seek = nullptr,
        const TrackPipelineOpenOptions& options = {}) const;
};

} // namespace vr
