#include "player/native_player.h"
#include "audio/audio_output_factory.h"

namespace vr {

NativePlayer::NativePlayer()
    : playback_(create_default_audio_output)
    , renderer_(playback_) {}

NativePlayer::~NativePlayer() {
    shutdown();
}

bool NativePlayer::initialize(const RendererConfig& config) {
    playback_.start_session();
    if (!renderer_.initialize(config)) {
        playback_.stop_session();
        return false;
    }
    return true;
}

void NativePlayer::shutdown() {
    renderer_.shutdown();
    playback_.stop_session();
}

} // namespace vr
