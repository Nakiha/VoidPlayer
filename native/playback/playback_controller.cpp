#include "playback/playback_controller.h"

namespace vr {

PlaybackController::PlaybackController() = default;

PlaybackController::~PlaybackController() {
    stop_session();
}

void PlaybackController::start_session() {
    stop_session();
    clock_.pause();
    clock_.seek(0);
    clock_.set_speed(1.0);
    audio_engine_ = std::make_unique<AudioEngine>();
}

void PlaybackController::stop_session() {
    if (audio_engine_) {
        audio_engine_->clear();
        audio_engine_.reset();
    }
    clock_.pause();
    clock_.seek(0);
    clock_.set_speed(1.0);
}

void PlaybackController::play() {
    if (audio_engine_) audio_engine_->play();
    clock_.resume();
}

void PlaybackController::pause() {
    if (audio_engine_) audio_engine_->pause();
    clock_.pause();
}

void PlaybackController::seek_clock(int64_t target_pts_us) {
    clock_.seek(target_pts_us);
}

void PlaybackController::set_speed(double speed) {
    clock_.set_speed(speed);
}

double PlaybackController::speed() const {
    return clock_.speed();
}

} // namespace vr
