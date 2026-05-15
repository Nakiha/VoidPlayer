#include <catch2/catch_test_macros.hpp>

#include "audio/audio_track_registry.h"
#include "audio/pcm_buffer.h"

#include <cstdint>
#include <memory>
#include <vector>

using namespace vr;

namespace {

class FakeAudioTrackController final : public AudioTrackController {
public:
    void stop() override {
        ++stop_count;
    }

    void set_paused(bool paused) override {
        pause_values.push_back(paused);
    }

    void notify_seek(int64_t target_pts_us, SeekType type) override {
        seek_targets.push_back(target_pts_us);
        seek_types.push_back(type);
    }

    int stop_count = 0;
    std::vector<bool> pause_values;
    std::vector<int64_t> seek_targets;
    std::vector<SeekType> seek_types;
};

std::shared_ptr<PcmBuffer> make_buffer() {
    return std::make_shared<PcmBuffer>(2, 48000, 480);
}

std::unique_ptr<FakeAudioTrackController> make_controller(
    FakeAudioTrackController** out) {
    auto controller = std::make_unique<FakeAudioTrackController>();
    *out = controller.get();
    return controller;
}

} // namespace

TEST_CASE("AudioTrackRegistry publishes track buffers", "[audio][registry]") {
    AudioTrackRegistry registry;
    FakeAudioTrackController* controller = nullptr;
    auto buffer = make_buffer();

    auto previous = registry.add_or_replace(
        7,
        buffer,
        make_controller(&controller));

    REQUIRE_FALSE(previous.has_value());
    REQUIRE(registry.size() == 1);
    const auto buffers = registry.buffers();
    REQUIRE(buffers.size() == 1);
    REQUIRE(buffers.at(7) == buffer);
    REQUIRE(controller != nullptr);
}

TEST_CASE("AudioTrackRegistry fans out pause and seek controls",
          "[audio][registry]") {
    AudioTrackRegistry registry;
    FakeAudioTrackController* first = nullptr;
    FakeAudioTrackController* second = nullptr;

    registry.add_or_replace(1, make_buffer(), make_controller(&first));
    registry.add_or_replace(2, make_buffer(), make_controller(&second));

    REQUIRE(registry.set_track_decode_paused(1, true));
    REQUIRE(first->pause_values == std::vector<bool>{true});
    REQUIRE(second->pause_values.empty());

    registry.set_all_decode_paused(false);
    REQUIRE(first->pause_values == std::vector<bool>{true, false});
    REQUIRE(second->pause_values == std::vector<bool>{false});

    REQUIRE(registry.notify_seek(2, 123000, SeekType::Exact));
    REQUIRE(second->seek_targets == std::vector<int64_t>{123000});
    REQUIRE(second->seek_types == std::vector<SeekType>{SeekType::Exact});
    REQUIRE_FALSE(registry.notify_seek(99, 1, SeekType::Keyframe));
}

TEST_CASE("AudioTrackRegistry removes and clears owned tracks",
          "[audio][registry]") {
    AudioTrackRegistry registry;
    FakeAudioTrackController* first = nullptr;
    FakeAudioTrackController* second = nullptr;

    registry.add_or_replace(1, make_buffer(), make_controller(&first));
    registry.add_or_replace(2, make_buffer(), make_controller(&second));

    auto removed = registry.remove(1);
    REQUIRE(removed.has_value());
    REQUIRE(removed->file_id == 1);
    REQUIRE(registry.size() == 1);
    REQUIRE(registry.buffers().count(1) == 0);

    removed->decoder->stop();
    REQUIRE(first->stop_count == 1);

    auto cleared = registry.clear();
    REQUIRE(cleared.size() == 1);
    REQUIRE(cleared[0].file_id == 2);
    REQUIRE(registry.empty());

    cleared[0].decoder->stop();
    REQUIRE(second->stop_count == 1);
}

TEST_CASE("AudioTrackRegistry replacement returns the old track",
          "[audio][registry]") {
    AudioTrackRegistry registry;
    FakeAudioTrackController* first = nullptr;
    FakeAudioTrackController* second = nullptr;
    auto first_buffer = make_buffer();
    auto second_buffer = make_buffer();

    registry.add_or_replace(5, first_buffer, make_controller(&first));
    auto previous = registry.add_or_replace(
        5,
        second_buffer,
        make_controller(&second));

    REQUIRE(previous.has_value());
    REQUIRE(previous->file_id == 5);
    REQUIRE(previous->buffer == first_buffer);
    REQUIRE(registry.buffers().at(5) == second_buffer);

    previous->decoder->stop();
    REQUIRE(first->stop_count == 1);
    REQUIRE(second->stop_count == 0);
}
