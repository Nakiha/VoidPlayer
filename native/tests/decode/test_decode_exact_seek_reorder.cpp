#include <catch2/catch_test_macros.hpp>

#include "renderer/decode/decode_exact_seek_reorder.h"

using namespace vr;

TEST_CASE("DecodeExactSeekReorder: does nothing when inactive",
          "[decode_thread][decode_exact_seek_reorder]") {
    int drained = 0;
    int published = 0;
    const auto result = handle_exact_seek_reorder_after_receive(
        DecodeExactSeekReorderState{
            false,
            3,
            true,
            0,
            false,
            true,
        },
        DecodeExactSeekReorderCallbacks{
            [&]() { ++drained; },
            []() { return size_t(0); },
            {},
            [&]() { ++published; },
            {},
            {},
        });

    REQUIRE_FALSE(result.drained_codec);
    REQUIRE_FALSE(result.published);
    REQUIRE(drained == 0);
    REQUIRE(published == 0);
}

TEST_CASE("DecodeExactSeekReorder: drains and publishes at EOF",
          "[decode_thread][decode_exact_seek_reorder]") {
    int drained = 0;
    int drain_logs = 0;
    int published = 0;
    int publish_logs = 0;
    const auto result = handle_exact_seek_reorder_after_receive(
        DecodeExactSeekReorderState{
            true,
            2,
            true,
            0,
            false,
            false,
        },
        DecodeExactSeekReorderCallbacks{
            [&]() { ++drained; },
            []() { return size_t(4); },
            [&](size_t count) {
                ++drain_logs;
                REQUIRE(count == 4);
            },
            [&]() { ++published; },
            []() { return std::optional<int64_t>{1'250'000}; },
            [&](std::optional<int64_t> pts_us) {
                ++publish_logs;
                REQUIRE(pts_us.has_value());
                REQUIRE(*pts_us == 1'250'000);
            },
        });

    REQUIRE(result.drained_codec);
    REQUIRE(result.published);
    REQUIRE(drained == 1);
    REQUIRE(drain_logs == 1);
    REQUIRE(published == 1);
    REQUIRE(publish_logs == 1);
}

TEST_CASE("DecodeExactSeekReorder: publishes preview-ready window without drain",
          "[decode_thread][decode_exact_seek_reorder]") {
    int drained = 0;
    int published = 0;
    const auto result = handle_exact_seek_reorder_after_receive(
        DecodeExactSeekReorderState{
            true,
            3,
            false,
            8,
            false,
            true,
        },
        DecodeExactSeekReorderCallbacks{
            [&]() { ++drained; },
            []() { return size_t(3); },
            {},
            [&]() { ++published; },
            []() { return std::optional<int64_t>{}; },
            {},
        });

    REQUIRE_FALSE(result.drained_codec);
    REQUIRE(result.published);
    REQUIRE(drained == 0);
    REQUIRE(published == 1);
}
