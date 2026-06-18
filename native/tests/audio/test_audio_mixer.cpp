#include <catch2/catch_test_macros.hpp>

#include "audio/audio_mixer.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <vector>

using namespace vr;

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

} // namespace

TEST_CASE("AudioMixer: paused render outputs silence without consuming PCM",
          "[audio][mixer]") {
    auto buffer = std::make_shared<PcmBuffer>(2, 1000, 100);
    const auto samples = stereo_frames({1, 2, 3, 4, 5, 6});
    REQUIRE(buffer->push(samples.data(), 6, 10000, 6000, buffer->current_serial()));

    AudioMixer mixer(2, 4);
    mixer.set_tracks({{1, buffer}});
    mixer.set_active_track(1);
    mixer.set_playing(false);

    std::array<int16_t, 6> out = {};
    mixer.render(out.data(), 3);

    for (int16_t sample : out) {
        REQUIRE(sample == 0);
    }
    REQUIRE(buffer->queued_frames() == 6);
    const PcmBufferStats stats = buffer->stats();
    REQUIRE(stats.discarded_frames == 0);
    REQUIRE(stats.underrun_frames == 0);
}

TEST_CASE("AudioMixer: playing render consumes the active track",
          "[audio][mixer]") {
    auto buffer = std::make_shared<PcmBuffer>(2, 1000, 100);
    const auto samples = stereo_frames({1, 2, 3, 4, 5, 6});
    REQUIRE(buffer->push(samples.data(), 6, 10000, 6000, buffer->current_serial()));

    AudioMixer mixer(2, 4);
    mixer.set_tracks({{1, buffer}});
    mixer.set_active_track(1);
    mixer.set_playing(true);

    std::array<int16_t, 6> out = {};
    mixer.render(out.data(), 3);

    REQUIRE(buffer->queued_frames() == 3);
    REQUIRE(buffer->stats().underrun_frames == 0);
}

TEST_CASE("AudioMixer: no active track mutes output without consuming PCM",
          "[audio][mixer]") {
    auto buffer = std::make_shared<PcmBuffer>(2, 1000, 100);
    const auto samples = stereo_frames({10, 20, 30, 40, 50, 60});
    REQUIRE(buffer->push(samples.data(), 6, 10000, 6000, buffer->current_serial()));

    AudioMixer mixer(2, 1);
    mixer.set_tracks({{0, buffer}});
    mixer.set_active_track(kAudioNoTrack);
    mixer.set_playing(true);

    std::array<int16_t, 6> muted = {};
    mixer.render(muted.data(), 3);

    for (int16_t sample : muted) {
        REQUIRE(sample == 0);
    }
    REQUIRE(buffer->queued_frames() == 6);
    REQUIRE(buffer->stats().discarded_frames == 0);

    mixer.set_active_track(0);
    std::array<int16_t, 4> audible = {};
    mixer.render(audible.data(), 2);

    REQUIRE(audible[0] == 0);
    REQUIRE(audible[1] == 0);
    REQUIRE(audible[2] == 20);
    REQUIRE(audible[3] == 20);
    REQUIRE(buffer->queued_frames() == 4);
}
