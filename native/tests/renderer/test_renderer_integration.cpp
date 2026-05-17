#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/renderer.h"
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <d3d11.h>
#include <dxgi.h>

using namespace vr;
using namespace vr::test;

// Helper: get the default DXGI adapter for headless mode.
// Returns nullptr if no adapter is available.
static IDXGIAdapter* get_default_adapter() {
    static Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (adapter) return adapter.Get();

    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), &factory);
    if (FAILED(hr)) return nullptr;

    hr = factory->EnumAdapters(0, &adapter);
    if (FAILED(hr)) return nullptr;

    return adapter.Get();
}

static std::string quote_arg(const std::string& value) {
    std::string quoted = "\"";
    for (char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += "\"";
    return quoted;
}

static int run_command(const std::string& command) {
#ifdef _WIN32
    return std::system(("\"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

static void append_file(const std::filesystem::path& src, std::ofstream& out) {
    std::ifstream in(src, std::ios::binary);
    REQUIRE(in.good());
    out << in.rdbuf();
    REQUIRE(out.good());
}

static std::filesystem::path make_dynamic_resolution_h264_fixture() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir =
        fs::temp_directory_path() / ("void_player_renderer_dynamic_res_" + std::to_string(stamp));
    fs::create_directories(dir);

    const fs::path seg_a = dir / "seg_a.h264";
    const fs::path seg_b = dir / "seg_b.h264";
    const fs::path combined = dir / "dynamic_res.h264";
    const std::string ffmpeg = FFMPEG_EXE_PATH;
    if (!fs::exists(ffmpeg)) {
        SKIP("FFmpeg CLI is not bundled with the minimal runtime package");
    }

    auto encode_segment = [&](const fs::path& out, const char* size) {
        std::ostringstream cmd;
        cmd << quote_arg(ffmpeg)
            << " -hide_banner -y -loglevel error"
            << " -f lavfi -i " << quote_arg(std::string("testsrc=size=") + size + ":rate=5:duration=1")
            << " -frames:v 5"
            << " -c:v libx264 -preset ultrafast -tune zerolatency"
            << " -g 5 -keyint_min 5 -x264-params " << quote_arg("scenecut=0:repeat-headers=1")
            << " -pix_fmt yuv420p -f h264 "
            << quote_arg(out.string());
        const int ret = run_command(cmd.str());
        REQUIRE(ret == 0);
        REQUIRE(fs::exists(out));
    };

    encode_segment(seg_a, "64x64");
    encode_segment(seg_b, "96x72");

    std::ofstream out(combined, std::ios::binary);
    REQUIRE(out.good());
    append_file(seg_a, out);
    append_file(seg_b, out);
    out.close();
    REQUIRE(fs::exists(combined));
    return combined;
}

struct CaptureStats {
    uint64_t hash = 1469598103934665603ull;
    double avg_luma = 0.0;
    double non_black_ratio = 0.0;
    int width = 0;
    int height = 0;
};

static CaptureStats analyze_bgra(const std::vector<uint8_t>& bgra, int width, int height) {
    CaptureStats stats;
    stats.width = width;
    stats.height = height;
    if (bgra.empty() || width <= 0 || height <= 0) return stats;

    uint64_t luma_sum = 0;
    size_t non_black = 0;
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixels; ++i) {
        const size_t off = i * 4;
        const uint8_t b = bgra[off + 0];
        const uint8_t g = bgra[off + 1];
        const uint8_t r = bgra[off + 2];
        stats.hash ^= b;
        stats.hash *= 1099511628211ull;
        stats.hash ^= g;
        stats.hash *= 1099511628211ull;
        stats.hash ^= r;
        stats.hash *= 1099511628211ull;
        const int luma = (77 * r + 150 * g + 29 * b) >> 8;
        luma_sum += static_cast<uint64_t>(luma);
        if (r > 8 || g > 8 || b > 8) {
            ++non_black;
        }
    }
    stats.avg_luma = static_cast<double>(luma_sum) / static_cast<double>(pixels);
    stats.non_black_ratio = static_cast<double>(non_black) / static_cast<double>(pixels);
    return stats;
}

static CaptureStats wait_for_non_black_capture(Renderer& renderer,
                                               std::chrono::milliseconds timeout =
                                                   std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    CaptureStats latest;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<uint8_t> bgra;
        int width = 0;
        int height = 0;
        if (renderer.capture_front_buffer(bgra, width, height)) {
            latest = analyze_bgra(bgra, width, height);
            if (latest.non_black_ratio >= 0.01 && latest.avg_luma >= 4.0) {
                return latest;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return latest;
}

static CaptureStats wait_for_changed_non_black_capture(Renderer& renderer,
                                                       uint64_t previous_hash,
                                                       std::chrono::milliseconds timeout =
                                                           std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    CaptureStats latest;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<uint8_t> bgra;
        int width = 0;
        int height = 0;
        if (renderer.capture_front_buffer(bgra, width, height)) {
            latest = analyze_bgra(bgra, width, height);
            if (latest.hash != previous_hash &&
                latest.non_black_ratio >= 0.01 &&
                latest.avg_luma >= 4.0) {
                return latest;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return latest;
}

static void require_visual_frame(const CaptureStats& stats) {
    INFO("capture " << stats.width << "x" << stats.height
                    << " avg_luma=" << stats.avg_luma
                    << " non_black=" << stats.non_black_ratio
                    << " hash=" << stats.hash);
    REQUIRE(stats.width > 0);
    REQUIRE(stats.height > 0);
    REQUIRE(stats.non_black_ratio >= 0.01);
    REQUIRE(stats.avg_luma >= 4.0);
}

// =============================================================================
// Windowed-mode tests (software decode)
// =============================================================================

TEST_CASE("Renderer: initialize with single H264 file", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE(renderer.initialize(config));
    REQUIRE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 1);
    REQUIRE(renderer.duration_us() > 0);

    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: duplicate initialize is rejected without tearing down the running renderer", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE(renderer.initialize(config));
    REQUIRE_FALSE(renderer.initialize(config));
    REQUIRE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 1);

    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: failed initialize rolls back resources and allows retry", "[renderer]") {
    Renderer renderer;

    RendererConfig bad_config;
    bad_config.video_paths = { video_test_dir() + "/missing-video.mp4" };
    bad_config.hwnd = create_hidden_window(640, 480);
    bad_config.width = 640;
    bad_config.height = 480;
    bad_config.use_hardware_decode = false;

    REQUIRE_FALSE(renderer.initialize(bad_config));
    REQUIRE_FALSE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 0);

    RendererConfig good_config = bad_config;
    good_config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };

    REQUIRE(renderer.initialize(good_config));
    REQUIRE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 1);

    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
    destroy_window(static_cast<HWND>(bad_config.hwnd));
}

TEST_CASE("Renderer: play and check PTS advances", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    renderer.initialize(config);
    renderer.play();

    // Wait for playback to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int64_t pts = renderer.current_pts_us();
    REQUIRE(pts > 0);

    renderer.shutdown();
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: dynamic resolution updates displayed track geometry",
          "[renderer][dynamic_resolution]") {
    const auto path = make_dynamic_resolution_h264_fixture();
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { path.string() };
    config.hwnd = create_hidden_window(320, 240);
    config.width = 320;
    config.height = 240;
    config.use_hardware_decode = false;

    REQUIRE(renderer.initialize(config));
    renderer.play();

    bool saw_updated_geometry = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline && !saw_updated_geometry) {
        for (const auto& info : renderer.track_infos()) {
            saw_updated_geometry =
                saw_updated_geometry || (info.width == 96 && info.height == 72);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    renderer.shutdown();
    destroy_window(static_cast<HWND>(config.hwnd));
    std::filesystem::remove_all(path.parent_path());

    REQUIRE(saw_updated_geometry);
}

TEST_CASE("Renderer: pause and play", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.hwnd = create_hidden_window(640, 480);
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = false;

    renderer.initialize(config);
    renderer.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    renderer.pause();
    int64_t pts_at_pause = renderer.current_pts_us();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int64_t pts_after_wait = renderer.current_pts_us();

    // PTS should not advance while paused
    REQUIRE(std::abs(pts_after_wait - pts_at_pause) < 10000);

    renderer.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int64_t pts_after_play = renderer.current_pts_us();
    REQUIRE(pts_after_play > pts_at_pause);

    renderer.shutdown();
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: multi-track initialization", "[renderer]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = {
        video_test_dir() + "/h264_9s_1920x1080.mp4",
        video_test_dir() + "/h265_10s_1920x1080.mp4"
    };
    config.hwnd = create_hidden_window(1280, 480);
    config.width = 1280;
    config.height = 480;
    config.use_hardware_decode = false;

    REQUIRE(renderer.initialize(config));
    REQUIRE(renderer.track_count() == 2);

    renderer.shutdown();
    destroy_window(static_cast<HWND>(config.hwnd));
}

TEST_CASE("Renderer: shutdown without play is safe", "[renderer]") {
    Renderer renderer;
    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
}

TEST_CASE("Renderer: texture sharing failure increments D3D11 metrics", "[renderer][hw]") {
    Renderer renderer;
    SharedTextureSnapshot snapshot;

    REQUIRE_FALSE(renderer.acquire_shared_texture(snapshot));
    auto metrics = renderer.d3d_backend_metrics();
    REQUIRE(metrics.texture_sharing_failure_count == 1);
    REQUIRE(metrics.device_lost_count == 0);
}

// =============================================================================
// Headless-mode tests (hardware decode — mirrors Flutter plugin setup)
// =============================================================================

TEST_CASE("Renderer: headless mode requires a DXGI adapter", "[renderer][hw]") {
    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.headless = true;
    config.backend.type = RendererBackendType::D3D11;
    config.backend.adapter = nullptr;
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = true;

    REQUIRE_FALSE(renderer.initialize(config));
    REQUIRE_FALSE(renderer.is_initialized());
}

TEST_CASE("Renderer: headless hw decode initialize", "[renderer][hw]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.headless = true;
    config.backend.type = RendererBackendType::D3D11;
    config.backend.adapter = adapter;
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = true;

    REQUIRE(renderer.initialize(config));
    REQUIRE(renderer.is_initialized());
    REQUIRE(renderer.track_count() == 1);
    REQUIRE(renderer.duration_us() > 0);

    // Verify the shared texture snapshot is available for Flutter consumption.
    SharedTextureSnapshot snapshot;
    REQUIRE(renderer.acquire_shared_texture(snapshot));
    REQUIRE(snapshot.type == SharedTextureHandleType::D3D11SharedHandle);
    REQUIRE(snapshot.texture != nullptr);
    REQUIRE(snapshot.handle != nullptr);
    static_cast<ID3D11Texture2D*>(snapshot.texture)->Release();
    auto metrics = renderer.d3d_backend_metrics();
    REQUIRE(metrics.device_lost_count == 0);
    REQUIRE(metrics.texture_sharing_failure_count == 0);

    renderer.shutdown();
    REQUIRE_FALSE(renderer.is_initialized());
}

TEST_CASE("Renderer: headless hw decode play", "[renderer][hw]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h264_9s_1920x1080.mp4" };
    config.headless = true;
    config.backend.type = RendererBackendType::D3D11;
    config.backend.adapter = adapter;
    config.width = 640;
    config.height = 480;
    config.use_hardware_decode = true;

    REQUIRE(renderer.initialize(config));
    renderer.play();

    // Wait for playback to start and a few frames to be decoded/rendered
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    int64_t pts = renderer.current_pts_us();
    REQUIRE(pts > 0);

    // Verify shared texture snapshot remains valid during playback.
    SharedTextureSnapshot snapshot;
    REQUIRE(renderer.acquire_shared_texture(snapshot));
    REQUIRE(snapshot.type == SharedTextureHandleType::D3D11SharedHandle);
    REQUIRE(snapshot.texture != nullptr);
    REQUIRE(snapshot.handle != nullptr);
    static_cast<ID3D11Texture2D*>(snapshot.texture)->Release();
    auto metrics = renderer.d3d_backend_metrics();
    REQUIRE(metrics.present_publish_count > 0);
    REQUIRE(metrics.frame_copy_count > 0);

    renderer.shutdown();
}

TEST_CASE("Renderer: headless hw decode multi-track", "[renderer][hw]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    Renderer renderer;

    RendererConfig config;
    config.video_paths = {
        video_test_dir() + "/h264_9s_1920x1080.mp4",
        video_test_dir() + "/h265_10s_1920x1080.mp4"
    };
    config.headless = true;
    config.backend.type = RendererBackendType::D3D11;
    config.backend.adapter = adapter;
    config.width = 1280;
    config.height = 480;
    config.use_hardware_decode = true;

    REQUIRE(renderer.initialize(config));
    REQUIRE(renderer.track_count() == 2);

    renderer.shutdown();
}

TEST_CASE("Renderer: headless HEVC paused exact seek updates captured frame", "[renderer][hw][seek][visual]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    Renderer renderer;

    RendererConfig config;
    config.video_paths = { video_test_dir() + "/h265_10s_1920x1080.mp4" };
    config.headless = true;
    config.backend.type = RendererBackendType::D3D11;
    config.backend.adapter = adapter;
    config.width = 1280;
    config.height = 720;
    config.use_hardware_decode = true;

    REQUIRE(renderer.initialize(config));

    auto initial = wait_for_non_black_capture(renderer);
    require_visual_frame(initial);

    renderer.seek(1000000, SeekType::Exact);
    auto seek_1s = wait_for_changed_non_black_capture(renderer, initial.hash);
    require_visual_frame(seek_1s);
    REQUIRE(seek_1s.hash != initial.hash);

    renderer.seek(3500000, SeekType::Exact);
    auto seek_3_5s = wait_for_changed_non_black_capture(renderer, seek_1s.hash);
    require_visual_frame(seek_3_5s);

    REQUIRE(seek_1s.hash != seek_3_5s.hash);

    renderer.step_forward();
    auto after_step_forward =
        wait_for_changed_non_black_capture(renderer, seek_3_5s.hash);
    require_visual_frame(after_step_forward);
    REQUIRE(after_step_forward.hash != seek_3_5s.hash);

    renderer.shutdown();
}

TEST_CASE("Renderer: headless AV1 and VP9 produce visual frames", "[renderer][hw][visual]") {
    auto* adapter = get_default_adapter();
    REQUIRE(adapter != nullptr);

    const std::vector<std::string> files = {
        video_test_dir() + "/av1_10s_1920x1080.webm",
        video_test_dir() + "/vp9_10s_1920x1080.webm",
    };

    for (const auto& file : files) {
        INFO("file=" << file);
        REQUIRE(std::filesystem::exists(file));

        Renderer renderer;
        RendererConfig config;
        config.video_paths = { file };
        config.headless = true;
        config.backend.type = RendererBackendType::D3D11;
        config.backend.adapter = adapter;
        config.width = 1280;
        config.height = 720;
        config.use_hardware_decode = true;

        REQUIRE(renderer.initialize(config));
        auto initial = wait_for_non_black_capture(renderer);
        require_visual_frame(initial);

        renderer.seek(3500000, SeekType::Exact);
        auto after_seek = wait_for_changed_non_black_capture(renderer, initial.hash);
        require_visual_frame(after_seek);
        REQUIRE(initial.hash != after_seek.hash);

        renderer.shutdown();
    }
}
