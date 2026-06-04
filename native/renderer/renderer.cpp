#include "renderer/renderer_internal.h"

namespace vr {

Renderer::Renderer()
    : owned_playback_(std::make_unique<PlaybackController>(create_default_audio_output))
    , playback_(owned_playback_.get())
    , audio_coordinator_(std::make_unique<AudioCoordinator>(*playback_))
    , seek_coordinator_(std::make_unique<SeekCoordinator>(kPausedHevcSeekSettleDelay))
    , analysis_overlay_renderer_(std::make_unique<AnalysisOverlayRenderer>()) {
#ifdef _WIN32
    frame_capture_ = new FrameCaptureService();
#endif
}

Renderer::Renderer(PlaybackController& playback)
    : playback_(&playback)
    , audio_coordinator_(std::make_unique<AudioCoordinator>(*playback_))
    , seek_coordinator_(std::make_unique<SeekCoordinator>(kPausedHevcSeekSettleDelay))
    , analysis_overlay_renderer_(std::make_unique<AnalysisOverlayRenderer>()) {
#ifdef _WIN32
    frame_capture_ = new FrameCaptureService();
#endif
}

Renderer::~Renderer() {
    shutdown();
#ifdef _WIN32
    delete frame_capture_;
    frame_capture_ = nullptr;
#endif
}

} // namespace vr
