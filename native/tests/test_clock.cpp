#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/clock.h"
#include <thread>
#include <chrono>

using namespace vr;
using MockTime = vr::test::MockTimeSource;

TEST_CASE("Clock: default state is paused at 0", "[clock]") {
    MockTime mt{0};
    Clock clock([&mt]() { return mt(); });
    REQUIRE(clock.is_paused() == true);
    REQUIRE(clock.speed() == 1.0);
    REQUIRE(clock.current_pts_us() == 0);
}

TEST_CASE("Clock: play updates state", "[clock]") {
    MockTime mt{0};
    Clock clock([&mt]() { return mt(); });
    clock.play();
    REQUIRE(clock.is_paused() == false);
}

TEST_CASE("Clock: pause freezes PTS", "[clock]") {
    MockTime mt{0};
    Clock clock([&mt]() { return mt(); });
    clock.play();

    mt.t = 100000;  // 100ms elapsed
    int64_t pts_a = clock.current_pts_us();

    clock.pause();
    mt.t = 500000;  // advance wall clock
    int64_t pts_b = clock.current_pts_us();

    REQUIRE(pts_a == pts_b);
    REQUIRE(clock.is_paused() == true);
}

TEST_CASE("Clock: resume continues from paused PTS", "[clock]") {
    MockTime mt{0};
    Clock clock([&mt]() { return mt(); });
    clock.play();

    mt.t = 100000;
    clock.pause();

    mt.t = 500000;
    clock.resume();

    mt.t = 600000;  // 100ms after resume
    int64_t pts = clock.current_pts_us();
    // Should be ~200ms (100ms before pause + 100ms after resume)
    REQUIRE(pts >= 190000);
    REQUIRE(pts <= 210000);
}

TEST_CASE("Clock: seek resets base PTS", "[clock]") {
    MockTime mt{0};
    Clock clock([&mt]() { return mt(); });
    clock.play();

    mt.t = 5000000;
    clock.seek(5000000);  // seek to 5s
    REQUIRE(clock.current_pts_us() == 5000000);
}

TEST_CASE("Clock: speed change preserves current PTS", "[clock]") {
    MockTime mt{0};
    Clock clock([&mt]() { return mt(); });
    clock.play();

    mt.t = 1000000;  // 1s elapsed -> PTS = 1s
    int64_t pts_before = clock.current_pts_us();

    clock.set_speed(2.0);
    int64_t pts_after = clock.current_pts_us();

    // PTS should not jump on speed change
    REQUIRE(std::abs(pts_after - pts_before) < 1000);
}

TEST_CASE("Clock: double speed advances twice as fast", "[clock]") {
    MockTime mt{0};
    Clock clock([&mt]() { return mt(); });
    clock.play();
    clock.set_speed(2.0);

    mt.t = 100000;  // 100ms wall clock
    int64_t pts = clock.current_pts_us();
    // At 2x, 100ms wall -> 200ms PTS
    REQUIRE(pts >= 190000);
    REQUIRE(pts <= 210000);
}

TEST_CASE("Clock: seek while paused", "[clock]") {
    MockTime mt{0};
    Clock clock([&mt]() { return mt(); });
    clock.seek(3000000);
    REQUIRE(clock.current_pts_us() == 3000000);
    REQUIRE(clock.is_paused() == true);
}

TEST_CASE("Clock: half speed advances half as fast", "[clock]") {
    MockTime mt{0};
    Clock clock([&mt]() { return mt(); });
    clock.play();
    clock.set_speed(0.5);

    mt.t = 200000;  // 200ms wall clock
    int64_t pts = clock.current_pts_us();
    // At 0.5x, 200ms wall -> 100ms PTS
    REQUIRE(pts >= 90000);
    REQUIRE(pts <= 110000);
}
