#include <catch2/catch_test_macros.hpp>

#include "test_utils.h"
#include "video_renderer/renderer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <d3d11.h>
#include <dxgi.h>
#include <random>
#include <thread>
#include <vector>
#include <wrl/client.h>

using namespace vr;
using namespace vr::test;

namespace {

IDXGIAdapter* get_default_adapter() {
    static Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (adapter) return adapter.Get();

    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), &factory);
    if (FAILED(hr)) return nullptr;

    hr = factory->EnumAdapters(0, &adapter);
    if (FAILED(hr)) return nullptr;

    return adapter.Get();
}

} // namespace

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

TEST_CASE("Renderer: shutdown during headless capture and resize stress",
          "[renderer][stress][capture]") {
    constexpr uint32_t kSeed = 0x5A17C0DEu;
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    INFO("seed=" << kSeed);

    for (int run = 0; run < 3; ++run) {
        INFO("run=" << run << " seed=" << kSeed);

        Renderer renderer;
        RendererConfig config;
        config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
        config.headless = true;
        config.backend.adapter = adapter;
        config.width = 320;
        config.height = 180;
        config.use_hardware_decode = false;

        REQUIRE(renderer.initialize(config));
        renderer.play();

        std::atomic<bool> stop{false};
        std::atomic<int> capture_calls{0};
        std::atomic<int> resize_calls{0};

        std::thread capture_thread([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                std::vector<uint8_t> bgra;
                int width = 0;
                int height = 0;
                (void)renderer.capture_front_buffer(bgra, width, height);
                capture_calls.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });

        std::thread resize_thread([&]() {
            const int sizes[][2] = {
                {320, 180},
                {352, 198},
                {384, 216},
                {416, 234},
            };
            int index = 0;
            while (!stop.load(std::memory_order_acquire)) {
                const auto& size = sizes[index % 4];
                renderer.resize(size[0], size[1]);
                resize_calls.fetch_add(1, std::memory_order_relaxed);
                ++index;
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        renderer.shutdown();
        stop.store(true, std::memory_order_release);
        capture_thread.join();
        resize_thread.join();

        REQUIRE(capture_calls.load(std::memory_order_relaxed) > 0);
        REQUIRE(resize_calls.load(std::memory_order_relaxed) > 0);
        REQUIRE_FALSE(renderer.is_initialized());

        std::vector<uint8_t> bgra;
        int width = 1;
        int height = 1;
        REQUIRE_FALSE(renderer.capture_front_buffer(bgra, width, height));
        REQUIRE(bgra.empty());
        REQUIRE(width == 0);
        REQUIRE(height == 0);
    }
}
