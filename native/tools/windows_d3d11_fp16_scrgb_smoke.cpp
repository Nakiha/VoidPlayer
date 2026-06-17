#include "windows/d3d11/render_backend.h"

#include "renderer/color/color_reference.h"
#include "renderer/frame/frame_storage.h"
#include "renderer/render/renderer_draw_snapshot.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {

struct Bgra {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
    uint8_t a = 255;
};

struct Rgba {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
};

struct Captures {
    std::vector<uint16_t> fp16;
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    int overlay_draw_count = 0;
    vr::PresentationBackendDiagnostics diagnostics;
};

bool env_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' &&
           std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0;
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> select_adapter() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return {};
    }
    const bool allow_software =
        env_flag_enabled("VOIDPLAYER_ALLOW_D3D11_HEADLESS_WARP_FALLBACK");
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr) || !adapter) {
            continue;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        const bool software =
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!software || allow_software) {
            return adapter;
        }
    }
    return {};
}

float half_to_float(uint16_t half) {
    const double sign = (half & 0x8000u) != 0 ? -1.0 : 1.0;
    const int exponent = static_cast<int>((half >> 10u) & 0x1fu);
    const int mantissa = static_cast<int>(half & 0x03ffu);
    if (exponent == 0) {
        return mantissa == 0
            ? static_cast<float>(sign * 0.0)
            : static_cast<float>(
                  sign * std::ldexp(static_cast<double>(mantissa), -24));
    }
    if (exponent == 0x1f) {
        return mantissa == 0
            ? static_cast<float>(
                  sign * std::numeric_limits<double>::infinity())
            : std::numeric_limits<float>::quiet_NaN();
    }
    const double value = 1.0 + static_cast<double>(mantissa) / 1024.0;
    return static_cast<float>(sign * std::ldexp(value, exponent - 15));
}

Rgba read_fp16(const Captures& captures, int x, int y) {
    const size_t offset =
        (static_cast<size_t>(y) * captures.width +
         static_cast<size_t>(x)) *
        4u;
    return {
        half_to_float(captures.fp16[offset]),
        half_to_float(captures.fp16[offset + 1]),
        half_to_float(captures.fp16[offset + 2]),
        half_to_float(captures.fp16[offset + 3]),
    };
}

Bgra read_bgra(const Captures& captures, int x, int y) {
    const size_t offset =
        (static_cast<size_t>(y) * captures.width +
         static_cast<size_t>(x)) *
        4u;
    return {
        captures.bgra[offset],
        captures.bgra[offset + 1],
        captures.bgra[offset + 2],
        captures.bgra[offset + 3],
    };
}

bool expect_near(const char* name,
                 const Rgba& actual,
                 const Rgba& expected,
                 double tolerance) {
    const double differences[] = {
        std::abs(actual.r - expected.r),
        std::abs(actual.g - expected.g),
        std::abs(actual.b - expected.b),
        std::abs(actual.a - expected.a),
    };
    if (differences[0] <= tolerance && differences[1] <= tolerance &&
        differences[2] <= tolerance && differences[3] <= tolerance) {
        return true;
    }
    std::fprintf(
        stderr,
        "%s mismatch: actual=(%.5f,%.5f,%.5f,%.5f) "
        "expected=(%.5f,%.5f,%.5f,%.5f) tolerance=%.5f\n",
        name,
        actual.r,
        actual.g,
        actual.b,
        actual.a,
        expected.r,
        expected.g,
        expected.b,
        expected.a,
        tolerance);
    return false;
}

bool expect_bgra(const char* name,
                 const Bgra& actual,
                 const Bgra& expected,
                 int tolerance = 0) {
    const int differences[] = {
        std::abs(static_cast<int>(actual.b) - expected.b),
        std::abs(static_cast<int>(actual.g) - expected.g),
        std::abs(static_cast<int>(actual.r) - expected.r),
        std::abs(static_cast<int>(actual.a) - expected.a),
    };
    if (differences[0] <= tolerance && differences[1] <= tolerance &&
        differences[2] <= tolerance && differences[3] <= tolerance) {
        return true;
    }
    std::fprintf(
        stderr,
        "%s BGRA mismatch: actual=(%u,%u,%u,%u) "
        "expected=(%u,%u,%u,%u) tolerance=%d\n",
        name,
        static_cast<unsigned>(actual.b),
        static_cast<unsigned>(actual.g),
        static_cast<unsigned>(actual.r),
        static_cast<unsigned>(actual.a),
        static_cast<unsigned>(expected.b),
        static_cast<unsigned>(expected.g),
        static_cast<unsigned>(expected.r),
        static_cast<unsigned>(expected.a),
        tolerance);
    return false;
}

vr::TextureFrame make_bgra_frame(int width,
                                 int height,
                                 Bgra color,
                                 int64_t pts_us) {
    auto data = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(width) * height * 4u);
    for (size_t index = 0; index < static_cast<size_t>(width) * height;
         ++index) {
        (*data)[index * 4u] = color.b;
        (*data)[index * 4u + 1] = color.g;
        (*data)[index * 4u + 2] = color.r;
        (*data)[index * 4u + 3] = color.a;
    }
    vr::TextureFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pts_us = pts_us;
    frame.duration_us = 16667;
    frame.texture_handle = data->data();
    frame.storage = vr::CpuRgbaFrameStorage{data, width * 4};
    return frame;
}

vr::TextureFrame make_p010_frame(uint16_t y10,
                                 uint16_t u10,
                                 uint16_t v10,
                                 int transfer,
                                 int primaries,
                                 int64_t pts_us) {
    constexpr int width = 5;
    constexpr int height = 3;
    constexpr int coded_width = 6;
    constexpr int coded_height = 4;
    constexpr int stride = 16;
    auto data = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(stride) * coded_height +
            static_cast<size_t>(stride) * (coded_height / 2),
        0);
    auto write10 = [](uint8_t* destination, uint16_t value) {
        const uint16_t packed = static_cast<uint16_t>(value << 6);
        destination[0] = static_cast<uint8_t>(packed & 0xffu);
        destination[1] = static_cast<uint8_t>(packed >> 8);
    };
    for (int y = 0; y < coded_height; ++y) {
        for (int x = 0; x < coded_width; ++x) {
            write10(data->data() + static_cast<size_t>(y) * stride + x * 2,
                    y10);
        }
    }
    uint8_t* uv =
        data->data() + static_cast<size_t>(stride) * coded_height;
    for (int y = 0; y < coded_height / 2; ++y) {
        for (int x = 0; x < coded_width / 2; ++x) {
            write10(uv + static_cast<size_t>(y) * stride + x * 4, u10);
            write10(
                uv + static_cast<size_t>(y) * stride + x * 4 + 2, v10);
        }
    }
    vr::TextureFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pts_us = pts_us;
    frame.duration_us = 16667;
    frame.is_nv12 = true;
    frame.is_p010 = true;
    frame.texture_handle = data->data();
    frame.color = {
        vr::VIDEO_COLOR_RANGE_LIMITED,
        vr::VIDEO_COLOR_MATRIX_BT2020_NCL,
        transfer,
        primaries,
    };
    frame.storage = vr::CpuNv12FrameStorage{
        data, stride, stride, true, coded_width, coded_height};
    return frame;
}

vr::RendererDrawSnapshot make_snapshot(
    const std::vector<vr::TextureFrame>& frames,
    int target_width,
    int target_height,
    const vr::LayoutState& layout) {
    vr::RendererDrawSnapshot snapshot;
    snapshot.decision.should_present = true;
    snapshot.decision.current_pts_us =
        frames.empty() ? 0 : frames.front().pts_us;
    snapshot.layout = layout;
    snapshot.target_width = target_width;
    snapshot.target_height = target_height;
    snapshot.background_color[3] = 1.0f;
    for (size_t slot = 0;
         slot < frames.size() && slot < vr::kMaxTracks;
         ++slot) {
        const auto& frame = frames[slot];
        snapshot.decision.frames[slot] = frame;
        snapshot.decision.file_ids[slot] = static_cast<int>(slot);
        snapshot.decision.track_generations[slot] = 1;
        snapshot.tracks[slot].active = true;
        snapshot.tracks[slot].file_id = static_cast<int>(slot);
        snapshot.tracks[slot].generation = 1;
        snapshot.tracks[slot].video_width = frame.width;
        snapshot.tracks[slot].video_height = frame.height;
        snapshot.tracks[slot].video_aspect =
            static_cast<float>(frame.width) / frame.height;
        snapshot.track_geometry[slot] = {
            true,
            frame.width,
            frame.height,
            snapshot.tracks[slot].video_aspect,
        };
    }
    return snapshot;
}

bool render_case(IDXGIAdapter* adapter,
                 const char* name,
                 double sdr_white_level_nits,
                 const vr::RendererDrawSnapshot& snapshot,
                 Captures& captures) {
    vr::D3D11RenderBackend backend;
    vr::PresentationBackendConfig config;
    config.adapter = adapter;
    config.width = snapshot.target_width;
    config.height = snapshot.target_height;
    config.max_track_slots = static_cast<int>(vr::kMaxTracks);
    config.headless = true;
    config.output_target = vr::ColorOutputTarget::kWindowsLinearScRGB;
    config.sdr_white_level_nits = sdr_white_level_nits;
    if (!backend.initialize(config) ||
        !backend.begin_renderer_managed_headless_frame()) {
        std::fprintf(stderr, "%s backend initialization failed\n", name);
        return false;
    }

    vr::PresentationBackendDrawHooks hooks;
    hooks.draw_overlay = [&captures](
                             vr::PresentationBackend&,
                             const vr::RendererDrawSnapshot&) {
        ++captures.overlay_draw_count;
    };
    if (!backend.draw_frame(snapshot, hooks)) {
        std::fprintf(stderr, "%s draw failed\n", name);
        return false;
    }
    auto callback = backend.publish_renderer_managed_headless_frame(name);
    if (callback) {
        callback();
    }

    int fp16_width = 0;
    int fp16_height = 0;
    if (!backend.capture_fp16_target(
            captures.fp16, fp16_width, fp16_height)) {
        std::fprintf(stderr, "%s FP16 capture failed\n", name);
        return false;
    }
    int bgra_width = 0;
    int bgra_height = 0;
    if (!backend.capture_front_buffer(
            captures.bgra, bgra_width, bgra_height)) {
        std::fprintf(stderr, "%s BGRA capture failed\n", name);
        return false;
    }
    captures.width = fp16_width;
    captures.height = fp16_height;
    captures.diagnostics = backend.diagnostics();
    if (fp16_width != bgra_width || fp16_height != bgra_height ||
        captures.overlay_draw_count != 2 ||
        !captures.diagnostics.fp16_target_active ||
        captures.diagnostics.target_format != "B8G8R8A8_UNORM" ||
        captures.diagnostics.render_target_format !=
            "R16G16B16A16_FLOAT" ||
        captures.diagnostics.sdr_compatibility_pass != "source-rerender" ||
        captures.diagnostics.fp16_draw_count != 1 ||
        captures.diagnostics.sdr_compatibility_draw_count != 1) {
        std::fprintf(stderr, "%s diagnostics/pass contract mismatch\n", name);
        return false;
    }
    return true;
}

uint16_t limited_luma_code(double encoded, int bit_depth) {
    const double normalized = (encoded * 219.0 + 16.0) / 255.0;
    const uint32_t maximum = (1u << bit_depth) - 1u;
    return static_cast<uint16_t>(
        std::clamp(std::lround(normalized * maximum),
                   0l,
                   static_cast<long>(maximum)));
}

uint16_t neutral_chroma_code(int bit_depth) {
    const uint32_t maximum = (1u << bit_depth) - 1u;
    return static_cast<uint16_t>(
        std::lround((128.0 / 255.0) * maximum));
}

double pq_encode_nits(double nits) {
    constexpr double m1 = 0.1593017578125;
    constexpr double m2 = 78.84375;
    constexpr double c1 = 0.8359375;
    constexpr double c2 = 18.8515625;
    constexpr double c3 = 18.6875;
    const double linear = std::clamp(nits / 10000.0, 0.0, 1.0);
    const double powered = std::pow(linear, m1);
    return std::pow((c1 + c2 * powered) / (1.0 + c3 * powered), m2);
}

double hlg_encode_linear(double linear) {
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    linear = std::max(linear, 0.0);
    if (linear <= 1.0 / 12.0) {
        return std::sqrt(3.0 * linear);
    }
    return a * std::log(12.0 * linear - b) + c;
}

vr::ColorReferenceRgb reference_p010(
    uint16_t y,
    uint16_t u,
    uint16_t v,
    int transfer,
    int primaries) {
    vr::ColorReferenceConfig config;
    config.range = vr::VIDEO_COLOR_RANGE_LIMITED;
    config.matrix = vr::VIDEO_COLOR_MATRIX_BT2020_NCL;
    config.transfer = transfer;
    config.primaries = primaries;
    config.output_target = vr::ColorOutputTarget::kWindowsLinearScRGB;
    config.sdr_white_level_nits = 80.0;
    const auto p010_unorm = [](uint16_t value) {
        return static_cast<double>(static_cast<uint32_t>(value) << 6u) /
            65535.0;
    };
    return vr::color_reference_sample_yuv(
        {
            p010_unorm(y),
            p010_unorm(u),
            p010_unorm(v),
        },
        config);
}

bool test_sdr_white_scale(IDXGIAdapter* adapter) {
    vr::LayoutState fill;
    fill.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
    const Bgra source{64, 128, 255, 255};
    const auto frame = make_bgra_frame(2, 2, source, 1000);
    for (const double white_nits : {80.0, 203.0}) {
        Captures captures;
        if (!render_case(
                adapter,
                white_nits == 80.0 ? "SDR white 80" : "SDR white 203",
                white_nits,
                make_snapshot({frame}, 2, 2, fill),
                captures)) {
            return false;
        }
        const double scale = white_nits / 80.0;
        const Rgba expected = {
            vr::color_reference_srgb_to_linear(source.r / 255.0) * scale,
            vr::color_reference_srgb_to_linear(source.g / 255.0) * scale,
            vr::color_reference_srgb_to_linear(source.b / 255.0) * scale,
            1.0,
        };
        if (!expect_near("SDR FP16 scale",
                         read_fp16(captures, 0, 0),
                         expected,
                         0.004) ||
            !expect_bgra(
                "SDR compatibility", read_bgra(captures, 0, 0), source)) {
            return false;
        }
    }
    return true;
}

bool test_hdr_reference(IDXGIAdapter* adapter) {
    vr::LayoutState fill;
    fill.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
    const uint16_t neutral = neutral_chroma_code(10);
    for (const double nits : {80.0, 1000.0}) {
        const uint16_t y = limited_luma_code(pq_encode_nits(nits), 10);
        auto frame = make_p010_frame(
            y,
            neutral,
            neutral,
            vr::VIDEO_COLOR_TRANSFER_PQ,
            vr::VIDEO_COLOR_PRIMARIES_BT2020,
            static_cast<int64_t>(nits));
        Captures captures;
        if (!render_case(
                adapter,
                nits == 80.0 ? "PQ 80 nits" : "PQ 1000 nits",
                80.0,
                make_snapshot({frame}, 5, 3, fill),
                captures)) {
            return false;
        }
        const Rgba actual = read_fp16(captures, 2, 1);
        const double expected_level = nits / 80.0;
        if (!expect_near(
                "PQ FP16 reference",
                actual,
                {expected_level, expected_level, expected_level, 1.0},
                nits == 80.0 ? 0.03 : 0.12)) {
            return false;
        }
        if (nits > 80.0 && actual.r <= 1.0) {
            std::fprintf(stderr, "PQ highlight was prematurely tone-mapped\n");
            return false;
        }
    }

    const double hlg_linear = 0.25;
    const uint16_t hlg_y =
        limited_luma_code(hlg_encode_linear(hlg_linear), 10);
    auto hlg = make_p010_frame(
        hlg_y,
        neutral,
        neutral,
        vr::VIDEO_COLOR_TRANSFER_HLG,
        vr::VIDEO_COLOR_PRIMARIES_BT2020,
        3000);
    Captures hlg_captures;
    if (!render_case(
            adapter,
            "HLG P010",
            80.0,
            make_snapshot({hlg}, 5, 3, fill),
            hlg_captures)) {
        return false;
    }
    if (!expect_near(
            "HLG FP16 reference",
            read_fp16(hlg_captures, 4, 2),
            {
                hlg_linear * 4.0 * 203.0 / 80.0,
                hlg_linear * 4.0 * 203.0 / 80.0,
                hlg_linear * 4.0 * 203.0 / 80.0,
                1.0,
            },
            0.03)) {
        return false;
    }

    const uint16_t color_y = limited_luma_code(pq_encode_nits(300.0), 10);
    const uint16_t color_u = 420;
    const uint16_t color_v = 720;
    auto bt2020 = make_p010_frame(
        color_y,
        color_u,
        color_v,
        vr::VIDEO_COLOR_TRANSFER_PQ,
        vr::VIDEO_COLOR_PRIMARIES_BT2020,
        4000);
    Captures color_captures;
    if (!render_case(
            adapter,
            "BT.2020 to BT.709",
            80.0,
            make_snapshot({bt2020}, 5, 3, fill),
            color_captures)) {
        return false;
    }
    const auto color_expected = reference_p010(
        color_y,
        color_u,
        color_v,
        vr::VIDEO_COLOR_TRANSFER_PQ,
        vr::VIDEO_COLOR_PRIMARIES_BT2020);
    return expect_near(
        "BT.2020 FP16 reference",
        read_fp16(color_captures, 1, 1),
        {color_expected.r, color_expected.g, color_expected.b, 1.0},
        0.2);
}

bool test_layout_and_background(IDXGIAdapter* adapter) {
    vr::LayoutState split;
    split.mode = vr::LAYOUT_SPLIT_SCREEN;
    split.split_pos = 0.5f;
    split.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
    split.order[0] = 0;
    split.order[1] = 1;
    const Bgra left_color{0, 0, 220, 255};
    const Bgra right_color{220, 0, 0, 255};
    auto left = make_bgra_frame(3, 2, left_color, 5000);
    auto right = make_bgra_frame(3, 2, right_color, 5000);
    Captures split_captures;
    if (!render_case(
            adapter,
            "split/order",
            80.0,
            make_snapshot({left, right}, 12, 4, split),
            split_captures)) {
        return false;
    }
    if (!expect_bgra(
            "split left", read_bgra(split_captures, 3, 1), left_color) ||
        !expect_bgra(
            "split right", read_bgra(split_captures, 8, 1), right_color)) {
        return false;
    }

    vr::LayoutState fill;
    fill.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
    auto tall = make_bgra_frame(2, 4, left_color, 6000);
    auto background_snapshot = make_snapshot({tall}, 8, 4, fill);
    background_snapshot.background_color[0] = 24.0f / 255.0f;
    background_snapshot.background_color[1] = 32.0f / 255.0f;
    background_snapshot.background_color[2] = 40.0f / 255.0f;
    Captures background_captures;
    if (!render_case(
            adapter,
            "aspect/background",
            203.0,
            background_snapshot,
            background_captures)) {
        return false;
    }
    const double scale = 203.0 / 80.0;
    return expect_near(
               "FP16 background",
               read_fp16(background_captures, 0, 1),
               {
                   vr::color_reference_srgb_to_linear(24.0 / 255.0) * scale,
                   vr::color_reference_srgb_to_linear(32.0 / 255.0) * scale,
                   vr::color_reference_srgb_to_linear(40.0 / 255.0) * scale,
                   1.0,
               },
               0.004) &&
        expect_bgra(
               "BGRA background",
               read_bgra(background_captures, 0, 1),
               {40, 32, 24, 255},
               1);
}

}  // namespace

int main() {
    auto adapter = select_adapter();
    if (!adapter) {
        std::fprintf(
            stderr,
            "No usable DXGI adapter. Hosted CI may explicitly set "
            "VOIDPLAYER_ALLOW_D3D11_HEADLESS_WARP_FALLBACK=1.\n");
        return 1;
    }
    return test_sdr_white_scale(adapter.Get()) &&
            test_hdr_reference(adapter.Get()) &&
            test_layout_and_background(adapter.Get())
        ? 0
        : 1;
}
