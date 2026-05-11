#include <catch2/catch_test_macros.hpp>

#include "test_utils.h"
#include "video_renderer/renderer.h"

#include <cstdint>
#include <random>

using namespace vr;
using namespace vr::test;

TEST_CASE("Renderer: deterministic playback control stress",
          "[renderer][stress]") {
    constexpr uint32_t kSeed = 0xC0DEC0DEu;
    std::mt19937 rng(kSeed);
    std::uniform_int_distribution<int> op_dist(0, 8);
    std::uniform_int_distribution<int64_t> seek_dist(0, 8'000'000);
    std::uniform_real_distribution<double> speed_dist(0.25, 4.0);
    std::uniform_real_distribution<float> zoom_dist(0.5f, 3.0f);
    std::uniform_real_distribution<float> offset_dist(-200.0f, 200.0f);

    INFO("seed=" << kSeed);

    Renderer renderer;
    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE(renderer.initialize(config));

    for (int i = 0; i < 40; ++i) {
        INFO("iteration=" << i << " seed=" << kSeed);
        switch (op_dist(rng)) {
        case 0:
            renderer.play();
            break;
        case 1:
            renderer.pause();
            break;
        case 2:
            renderer.seek(seek_dist(rng), SeekType::Keyframe);
            break;
        case 3:
            renderer.seek(seek_dist(rng), SeekType::Exact);
            break;
        case 4:
            renderer.set_speed(speed_dist(rng));
            break;
        case 5:
            renderer.set_loop_range(true, 500'000, 2'000'000);
            break;
        case 6:
            renderer.set_loop_range(false, 0, 0);
            break;
        case 7: {
            LayoutState layout = renderer.layout();
            layout.zoom_ratio = zoom_dist(rng);
            layout.view_offset[0] = offset_dist(rng);
            layout.view_offset[1] = offset_dist(rng);
            renderer.apply_layout(layout);
            break;
        }
        case 8:
            renderer.step_forward();
            renderer.step_backward();
            break;
        default:
            break;
        }

        REQUIRE(renderer.is_initialized());
        REQUIRE(renderer.track_count() == 1);
        REQUIRE(renderer.current_speed() > 0.0);
    }

    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
    destroy_window(static_cast<HWND>(config.hwnd));
}
