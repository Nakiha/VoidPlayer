#include "playback/playback_controller.h"
#include <utility>

namespace vr {

PlaybackController::PlaybackController() = default;

PlaybackController::PlaybackController(AudioOutputFactory audio_output_factory)
    : audio_output_factory_(std::move(audio_output_factory)) {}

PlaybackController::~PlaybackController() {
    stop_session();
}

void PlaybackController::start_session() {
    stop_session();
    pacing_held_ = false;
    clock_.pause();
    clock_.seek(0);
    clock_.set_speed(1.0);
    if (audio_output_factory_) {
        audio_output_ = audio_output_factory_();
    }
}

void PlaybackController::stop_session() {
    if (audio_output_) {
        audio_output_->clear();
        audio_output_.reset();
    }
    clock_.pause();
    clock_.seek(0);
    clock_.set_speed(1.0);
    pacing_held_ = false;
}

void PlaybackController::play() {
    if (!pacing_held_) {
        if (audio_output_) audio_output_->play();
        clock_.resume();
    }
}

void PlaybackController::pause() {
    if (audio_output_) audio_output_->pause();
    clock_.pause();
}

void PlaybackController::seek_clock(int64_t target_pts_us) {
    clock_.seek(target_pts_us);
}

void PlaybackController::set_speed(double speed) {
    clock_.set_speed(speed);
}

void PlaybackController::set_effective_speed(double speed) {
    clock_.set_effective_speed(speed);
}

void PlaybackController::hold_for_pacing() {
    if (pacing_held_) {
        return;
    }
    pacing_held_ = true;
    if (audio_output_) {
        audio_output_->pause();
    }
    clock_.pause();
}

void PlaybackController::release_pacing_hold() {
    if (!pacing_held_) {
        return;
    }
    pacing_held_ = false;
    clock_.resume();
    if (audio_output_) {
        audio_output_->play();
    }
}

double PlaybackController::speed() const {
    return clock_.speed();
}

double PlaybackController::effective_speed() const {
    return clock_.effective_speed();
}

} // namespace vr
