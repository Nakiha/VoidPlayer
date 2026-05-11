#include "player/native_player.h"
#include "audio/audio_output_factory.h"
#include "video_renderer/renderer_config_validation.h"

namespace vr {

NativePlayer::NativePlayer()
    : playback_(create_default_audio_output)
    , renderer_(playback_) {}

NativePlayer::~NativePlayer() {
    shutdown();
}

bool NativePlayer::initialize(const RendererConfig& config) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_ != State::Created || renderer_.is_initialized()) {
        return false;
    }

    if (!validate_renderer_config(config)) {
        return false;
    }

    state_ = State::Initializing;
    playback_.start_session();
    if (!renderer_.initialize(config)) {
        playback_.stop_session();
        state_ = State::Created;
        return false;
    }
    state_ = State::Initialized;
    return true;
}

void NativePlayer::shutdown() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_ == State::ShuttingDown) {
        return;
    }
    state_ = State::ShuttingDown;
    renderer_.shutdown();
    playback_.stop_session();
    state_ = State::Created;
}

} // namespace vr
