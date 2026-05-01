#include "player/native_player.h"

namespace vr {

NativePlayer::NativePlayer()
    : renderer_(playback_) {}

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
