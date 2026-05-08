#include <catch2/catch_test_macros.hpp>

#include "audio/pcm_buffer.h"

#include <array>
#include <cstdint>
#include <initializer_list>
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

TEST_CASE("PcmBuffer: pop preserves PTS metadata and reports underrun", "[audio][pcm]") {
    PcmBuffer buffer(2, 1000, 100);
    const auto samples = stereo_frames({1, 2, 3, 4});

    REQUIRE(buffer.push(samples.data(), 4, 10000, 4000, buffer.current_serial()));

    std::array<int16_t, 12> out = {};
    const PcmPopResult result = buffer.pop(out.data(), 6);

    REQUIRE(result.requested_frames == 6);
    REQUIRE(result.pcm_frames == 4);
    REQUIRE(result.silence_frames == 2);
    REQUIRE(result.first_pts_us == 10000);
    REQUIRE(out[0] == 1);
    REQUIRE(out[6] == 4);
    REQUIRE(out[8] == 0);
    REQUIRE(out[11] == 0);

    const PcmBufferStats stats = buffer.stats();
    REQUIRE(stats.underrun_frames == 2);
    REQUIRE(stats.last_output_pts_us == 10000);
}

TEST_CASE("PcmBuffer: seek serial drops stale chunks and trims to target PTS", "[audio][pcm]") {
    PcmBuffer buffer(2, 1000, 100);
    const uint64_t old_serial = buffer.current_serial();
    buffer.begin_seek(12000, SeekType::Exact);
    const uint64_t new_serial = buffer.current_serial();

    const auto old_samples = stereo_frames({9, 9});
    REQUIRE_FALSE(buffer.push(old_samples.data(), 2, 10000, 2000, old_serial));

    const auto samples = stereo_frames({1, 2, 3, 4, 5, 6});
    REQUIRE(buffer.push(samples.data(), 6, 10000, 6000, new_serial));

    std::array<int16_t, 8> out = {};
    const PcmPopResult result = buffer.pop(out.data(), 4);

    REQUIRE(result.pcm_frames == 4);
    REQUIRE(result.first_pts_us == 12000);
    REQUIRE(out[0] == 3);
    REQUIRE(out[2] == 4);
    REQUIRE(out[6] == 6);

    const PcmBufferStats stats = buffer.stats();
    REQUIRE(stats.stale_chunks_dropped == 1);
    REQUIRE(stats.seek_trimmed_frames == 2);
    REQUIRE(stats.discarded_frames == 2);
}

TEST_CASE("PcmBuffer: exact seek fills small leading gap with silence", "[audio][pcm]") {
    PcmBuffer buffer(2, 1000, 100);
    buffer.begin_seek(12000, SeekType::Exact);

    const auto samples = stereo_frames({7, 8});
    REQUIRE(buffer.push(samples.data(), 2, 14000, 2000, buffer.current_serial()));

    std::array<int16_t, 8> out = {};
    const PcmPopResult result = buffer.pop(out.data(), 4);

    REQUIRE(result.pcm_frames == 4);
    REQUIRE(result.first_pts_us == 12000);
    REQUIRE(out[0] == 0);
    REQUIRE(out[2] == 0);
    REQUIRE(out[4] == 7);
    REQUIRE(out[6] == 8);
    REQUIRE(buffer.stats().silence_frames_inserted == 2);
}

TEST_CASE("PcmBuffer: drift metric records gaps between emitted chunks", "[audio][pcm]") {
    PcmBuffer buffer(2, 1000, 100);

    const auto first = stereo_frames({1, 2});
    REQUIRE(buffer.push(first.data(), 2, 10000, 2000, buffer.current_serial()));
    std::array<int16_t, 4> out = {};
    buffer.pop(out.data(), 2);

    const auto second = stereo_frames({3, 4});
    REQUIRE(buffer.push(second.data(), 2, 14000, 2000, buffer.current_serial()));
    buffer.pop(out.data(), 2);

    const PcmBufferStats stats = buffer.stats();
    REQUIRE(stats.last_drift_us == 2000);
    REQUIRE(stats.max_abs_drift_us == 2000);
}
