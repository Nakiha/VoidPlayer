#include "audio/audio_mixer.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

namespace {

std::vector<int16_t> stereo_frames(std::initializer_list<int16_t> values) {
    std::vector<int16_t> samples;
    samples.reserve(values.size() * 2);
    for (const int16_t value : values) {
        samples.push_back(value);
        samples.push_back(value);
    }
    return samples;
}

bool all_zero(const std::array<int16_t, 6>& samples) {
    for (const int16_t sample : samples) {
        if (sample != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    auto buffer = std::make_shared<vr::PcmBuffer>(2, 1000, 100);
    const auto samples = stereo_frames({10, 20, 30, 40, 50, 60});
    if (!buffer->push(samples.data(), 6, 10000, 6000, buffer->current_serial())) {
        std::cerr << "failed to push PCM samples\n";
        return 1;
    }

    vr::AudioMixer mixer(2, 1);
    mixer.set_tracks({{0, buffer}});
    mixer.set_active_track(vr::kAudioNoTrack);
    mixer.set_playing(true);

    std::array<int16_t, 6> muted = {};
    mixer.render(muted.data(), 3);
    if (!all_zero(muted) || buffer->queued_frames() != 6 ||
        buffer->stats().discarded_frames != 0) {
        std::cerr << "inactive audible track consumed or emitted PCM\n";
        return 1;
    }

    mixer.set_active_track(0);
    std::array<int16_t, 4> audible = {};
    mixer.render(audible.data(), 2);
    if (audible[0] != 0 || audible[1] != 0 ||
        audible[2] != 20 || audible[3] != 20 ||
        buffer->queued_frames() != 4) {
        std::cerr << "active audible track did not resume PCM output\n";
        return 1;
    }

    std::cout << "audio mixer audible-track smoke passed\n";
    return 0;
}
