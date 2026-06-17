#include "windows/d3d11/render_backend.h"

#include "renderer/frame/frame_storage.h"
#include "renderer/render/presentation_snapshot.h"
#include "renderer/render/renderer_draw_snapshot.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <vector>

namespace {

struct Bgra {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
    uint8_t a = 255;
};

struct Sample {
    int x = 0;
    int y = 0;
    Bgra expected;
};

struct PlanarOwner {
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
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

uint8_t clamp_u8(double value) {
    return static_cast<uint8_t>(
        std::clamp(static_cast<int>(std::lround(value * 255.0)), 0, 255));
}

Bgra reference_yuv_to_bgra(double y_sample,
                           double u_sample,
                           double v_sample,
                           int range,
                           int matrix) {
    double y_full = y_sample;
    double cb = (u_sample * 255.0 - 128.0) / 255.0;
    double cr = (v_sample * 255.0 - 128.0) / 255.0;
    if (range != vr::VIDEO_COLOR_RANGE_FULL) {
        y_full = (y_sample * 255.0 - 16.0) / 219.0;
        cb = (u_sample * 255.0 - 128.0) / 224.0;
        cr = (v_sample * 255.0 - 128.0) / 224.0;
    }

    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    if (matrix == vr::VIDEO_COLOR_MATRIX_BT2020_NCL) {
        r = y_full + 1.4746 * cr;
        g = y_full - 0.164553 * cb - 0.571353 * cr;
        b = y_full + 1.8814 * cb;
    } else if (matrix == vr::VIDEO_COLOR_MATRIX_BT601) {
        r = y_full + 1.402 * cr;
        g = y_full - 0.344136 * cb - 0.714136 * cr;
        b = y_full + 1.772 * cb;
    } else {
        r = y_full + 1.5748 * cr;
        g = y_full - 0.187324 * cb - 0.468124 * cr;
        b = y_full + 1.8556 * cb;
    }

    constexpr double kSdrBias = 1.0 / 255.0;
    return {
        clamp_u8(std::clamp(b - kSdrBias, 0.0, 1.0)),
        clamp_u8(std::clamp(g - kSdrBias, 0.0, 1.0)),
        clamp_u8(std::clamp(r - kSdrBias, 0.0, 1.0)),
        255,
    };
}

Bgra read_bgra(const std::vector<uint8_t>& data, int width, int x, int y) {
    const size_t offset =
        (static_cast<size_t>(y) * static_cast<size_t>(width) +
         static_cast<size_t>(x)) *
        4u;
    return {data[offset], data[offset + 1], data[offset + 2], data[offset + 3]};
}

bool expect_samples(const char* name,
                    const std::vector<uint8_t>& data,
                    int width,
                    const std::vector<Sample>& samples,
                    int tolerance) {
    for (const auto& sample : samples) {
        const Bgra actual = read_bgra(data, width, sample.x, sample.y);
        const int diffs[] = {
            std::abs(static_cast<int>(actual.b) - sample.expected.b),
            std::abs(static_cast<int>(actual.g) - sample.expected.g),
            std::abs(static_cast<int>(actual.r) - sample.expected.r),
            std::abs(static_cast<int>(actual.a) - sample.expected.a),
        };
        if (diffs[0] > tolerance || diffs[1] > tolerance ||
            diffs[2] > tolerance || diffs[3] > tolerance) {
            std::fprintf(
                stderr,
                "%s failed at (%d,%d): actual=(%u,%u,%u,%u) "
                "expected=(%u,%u,%u,%u) tolerance=%d\n",
                name,
                sample.x,
                sample.y,
                static_cast<unsigned>(actual.b),
                static_cast<unsigned>(actual.g),
                static_cast<unsigned>(actual.r),
                static_cast<unsigned>(actual.a),
                static_cast<unsigned>(sample.expected.b),
                static_cast<unsigned>(sample.expected.g),
                static_cast<unsigned>(sample.expected.r),
                static_cast<unsigned>(sample.expected.a),
                tolerance);
            return false;
        }
    }
    return true;
}

vr::TextureFrame make_bgra_frame(int width,
                                 int height,
                                 std::initializer_list<Bgra> pixels,
                                 int64_t pts_us) {
    auto data = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(width) * height * 4u, 0);
    size_t index = 0;
    for (const auto& pixel : pixels) {
        if (index >= static_cast<size_t>(width) * height) {
            break;
        }
        (*data)[index * 4u] = pixel.b;
        (*data)[index * 4u + 1] = pixel.g;
        (*data)[index * 4u + 2] = pixel.r;
        (*data)[index * 4u + 3] = pixel.a;
        ++index;
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

vr::TextureFrame make_nv12_frame(int width,
                                 int height,
                                 int coded_width,
                                 int coded_height,
                                 int y_stride,
                                 int uv_stride,
                                 uint8_t y_value,
                                 uint8_t u_value,
                                 uint8_t v_value,
                                 int range,
                                 int matrix,
                                 int64_t pts_us) {
    auto data = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(y_stride) * coded_height +
            static_cast<size_t>(uv_stride) * ((coded_height + 1) / 2),
        0);
    for (int y = 0; y < coded_height; ++y) {
        std::fill_n(data->data() + static_cast<size_t>(y) * y_stride,
                    coded_width,
                    y_value);
    }
    uint8_t* uv = data->data() + static_cast<size_t>(y_stride) * coded_height;
    for (int y = 0; y < (coded_height + 1) / 2; ++y) {
        uint8_t* row = uv + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < (coded_width + 1) / 2; ++x) {
            row[x * 2] = u_value;
            row[x * 2 + 1] = v_value;
        }
    }
    vr::TextureFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pts_us = pts_us;
    frame.duration_us = 16667;
    frame.is_nv12 = true;
    frame.texture_handle = data->data();
    frame.color = {range, matrix, vr::VIDEO_COLOR_TRANSFER_SDR,
                   vr::default_presentation_color_primaries_for_matrix(matrix)};
    frame.storage = vr::CpuNv12FrameStorage{
        data, y_stride, uv_stride, false, coded_width, coded_height};
    return frame;
}

vr::TextureFrame make_p010_frame(uint16_t y10,
                                 uint16_t u10,
                                 uint16_t v10,
                                 int64_t pts_us) {
    constexpr int width = 4;
    constexpr int height = 4;
    constexpr int stride = width * 2;
    auto data = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(stride) * height +
            static_cast<size_t>(stride) * (height / 2),
        0);
    auto write10 = [](uint8_t* destination, uint16_t value) {
        const uint16_t packed = static_cast<uint16_t>(value << 6);
        destination[0] = static_cast<uint8_t>(packed & 0xffu);
        destination[1] = static_cast<uint8_t>(packed >> 8);
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            write10(data->data() + static_cast<size_t>(y) * stride + x * 2, y10);
        }
    }
    uint8_t* uv = data->data() + static_cast<size_t>(stride) * height;
    for (int y = 0; y < height / 2; ++y) {
        for (int x = 0; x < width / 2; ++x) {
            write10(uv + static_cast<size_t>(y) * stride + x * 4, u10);
            write10(uv + static_cast<size_t>(y) * stride + x * 4 + 2, v10);
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
        vr::VIDEO_COLOR_MATRIX_BT709,
        vr::VIDEO_COLOR_TRANSFER_SDR,
        vr::VIDEO_COLOR_PRIMARIES_BT709,
    };
    frame.storage =
        vr::CpuNv12FrameStorage{data, stride, stride, true, width, height};
    return frame;
}

vr::TextureFrame make_planar_frame(int range,
                                   int matrix,
                                   uint8_t y_value,
                                   uint8_t u_value,
                                   uint8_t v_value,
                                   int64_t pts_us) {
    constexpr int width = 5;
    constexpr int height = 3;
    constexpr int coded_width = 6;
    constexpr int coded_height = 4;
    constexpr int y_stride = 8;
    constexpr int uv_stride = 4;
    auto owner = std::make_shared<PlanarOwner>();
    owner->y.assign(y_stride * coded_height, 0);
    owner->u.assign(uv_stride * ((coded_height + 1) / 2), 0);
    owner->v.assign(uv_stride * ((coded_height + 1) / 2), 0);
    for (int y = 0; y < coded_height; ++y) {
        std::fill_n(owner->y.data() + y * y_stride, coded_width, y_value);
    }
    for (int y = 0; y < (coded_height + 1) / 2; ++y) {
        std::fill_n(owner->u.data() + y * uv_stride, 3, u_value);
        std::fill_n(owner->v.data() + y * uv_stride, 3, v_value);
    }
    vr::CpuPlanarYuvFrameStorage storage;
    storage.frame_ref = owner;
    storage.planes[0] = owner->y.data();
    storage.planes[1] = owner->u.data();
    storage.planes[2] = owner->v.data();
    storage.strides[0] = y_stride;
    storage.strides[1] = uv_stride;
    storage.strides[2] = uv_stride;
    storage.plane_widths[0] = coded_width;
    storage.plane_widths[1] = 3;
    storage.plane_widths[2] = 3;
    storage.plane_heights[0] = coded_height;
    storage.plane_heights[1] = 2;
    storage.plane_heights[2] = 2;

    vr::TextureFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pts_us = pts_us;
    frame.duration_us = 16667;
    frame.texture_handle = owner->y.data();
    frame.color = {range, matrix, vr::VIDEO_COLOR_TRANSFER_SDR,
                   vr::default_presentation_color_primaries_for_matrix(matrix)};
    frame.storage = storage;
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

bool run_case(IDXGIAdapter* adapter,
              const char* name,
              int target_width,
              int target_height,
              const vr::RendererDrawSnapshot& snapshot,
              const std::vector<Sample>& samples,
              int tolerance = 4) {
    vr::D3D11RenderBackend backend;
    vr::PresentationBackendConfig config;
    config.adapter = adapter;
    config.width = target_width;
    config.height = target_height;
    config.max_track_slots = static_cast<int>(vr::kMaxTracks);
    config.headless = true;
    if (!backend.initialize(config)) {
        std::fprintf(stderr, "%s could not initialize D3D11 backend\n", name);
        return false;
    }
    const auto diagnostics = backend.diagnostics();
    if (diagnostics.target_format != "B8G8R8A8_UNORM" ||
        diagnostics.width != target_width ||
        diagnostics.height != target_height ||
        diagnostics.buffer_count != vr::D3D11HeadlessOutput::kBufferCount) {
        std::fprintf(stderr, "%s backend diagnostics mismatch\n", name);
        return false;
    }
    if (!backend.begin_renderer_managed_headless_frame()) {
        std::fprintf(stderr, "%s could not acquire headless frame\n", name);
        return false;
    }
    vr::PresentationBackendDrawHooks hooks;
    if (!backend.draw_frame(snapshot, hooks)) {
        std::fprintf(stderr, "%s draw_frame failed\n", name);
        return false;
    }
    auto callback = backend.publish_renderer_managed_headless_frame(name);
    if (callback) {
        callback();
    }

    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    if (!backend.capture_front_buffer(bgra, width, height) ||
        width != target_width || height != target_height) {
        std::fprintf(stderr, "%s capture failed\n", name);
        return false;
    }
    return expect_samples(name, bgra, width, samples, tolerance);
}

bool run_all(IDXGIAdapter* adapter) {
    vr::LayoutState fill;
    fill.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;

    auto bgra = make_bgra_frame(
        2,
        2,
        {{10, 20, 200, 255},
         {30, 180, 40, 255},
         {220, 50, 60, 255},
         {70, 80, 90, 255}},
        1000);
    if (!run_case(adapter,
                  "BGRA channel/order",
                  2,
                  2,
                  make_snapshot({bgra}, 2, 2, fill),
                  {{0, 0, {10, 20, 200, 255}},
                   {1, 0, {30, 180, 40, 255}},
                   {0, 1, {220, 50, 60, 255}},
                   {1, 1, {70, 80, 90, 255}}},
                  0)) {
        return false;
    }

    constexpr uint8_t nv_y = 180;
    constexpr uint8_t nv_u = 90;
    constexpr uint8_t nv_v = 200;
    const auto nv_expected = reference_yuv_to_bgra(
        nv_y / 255.0,
        nv_u / 255.0,
        nv_v / 255.0,
        vr::VIDEO_COLOR_RANGE_LIMITED,
        vr::VIDEO_COLOR_MATRIX_BT709);
    auto nv12 = make_nv12_frame(
        5,
        3,
        6,
        4,
        8,
        8,
        nv_y,
        nv_u,
        nv_v,
        vr::VIDEO_COLOR_RANGE_LIMITED,
        vr::VIDEO_COLOR_MATRIX_BT709,
        2000);
    if (!run_case(adapter,
                  "NV12 limited BT.709 odd dimensions/padded stride",
                  5,
                  3,
                  make_snapshot({nv12}, 5, 3, fill),
                  {{0, 0, nv_expected}, {4, 2, nv_expected}})) {
        return false;
    }

    const struct {
        const char* name;
        int range;
        int matrix;
        uint8_t y;
        uint8_t u;
        uint8_t v;
    } planar_cases[] = {
        {"YUV420P full BT.601",
         vr::VIDEO_COLOR_RANGE_FULL,
         vr::VIDEO_COLOR_MATRIX_BT601,
         170,
         100,
         190},
        {"YUV420P limited BT.709",
         vr::VIDEO_COLOR_RANGE_LIMITED,
         vr::VIDEO_COLOR_MATRIX_BT709,
         150,
         90,
         210},
    };
    int64_t pts_us = 3000;
    for (const auto& test : planar_cases) {
        const auto expected = reference_yuv_to_bgra(
            test.y / 255.0,
            test.u / 255.0,
            test.v / 255.0,
            test.range,
            test.matrix);
        auto frame = make_planar_frame(
            test.range, test.matrix, test.y, test.u, test.v, pts_us++);
        if (!run_case(adapter,
                      test.name,
                      5,
                      3,
                      make_snapshot({frame}, 5, 3, fill),
                      {{1, 1, expected}, {4, 2, expected}})) {
            return false;
        }
    }

    constexpr uint16_t p_y = 640;
    constexpr uint16_t p_u = 384;
    constexpr uint16_t p_v = 768;
    const auto p_expected = reference_yuv_to_bgra(
        p_y / 1023.0,
        p_u / 1023.0,
        p_v / 1023.0,
        vr::VIDEO_COLOR_RANGE_LIMITED,
        vr::VIDEO_COLOR_MATRIX_BT709);
    auto p010 = make_p010_frame(p_y, p_u, p_v, 4000);
    if (!run_case(adapter,
                  "P010 high-bit limited BT.709",
                  4,
                  4,
                  make_snapshot({p010}, 4, 4, fill),
                  {{1, 1, p_expected}, {3, 3, p_expected}},
                  5)) {
        return false;
    }

    auto aspect = make_bgra_frame(
        2,
        4,
        {{0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255}},
        5000);
    auto aspect_snapshot = make_snapshot({aspect}, 8, 4, fill);
    aspect_snapshot.background_color[0] = 24.0f / 255.0f;
    aspect_snapshot.background_color[1] = 32.0f / 255.0f;
    aspect_snapshot.background_color[2] = 40.0f / 255.0f;
    if (!run_case(adapter,
                  "layout aspect fit themed bars",
                  8,
                  4,
                  aspect_snapshot,
                  {{0, 1, {40, 32, 24, 255}},
                   {3, 1, {0, 0, 220, 255}},
                   {4, 2, {0, 0, 220, 255}},
                   {7, 1, {40, 32, 24, 255}}},
                  1)) {
        return false;
    }

    vr::LayoutState split;
    split.mode = vr::LAYOUT_SPLIT_SCREEN;
    split.split_pos = 0.5f;
    split.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
    split.order[0] = 0;
    split.order[1] = 1;
    auto left = make_bgra_frame(
        3,
        2,
        {{0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255},
         {0, 0, 220, 255}},
        6000);
    auto right = make_bgra_frame(
        3,
        2,
        {{220, 0, 0, 255},
         {220, 0, 0, 255},
         {220, 0, 0, 255},
         {220, 0, 0, 255},
         {220, 0, 0, 255},
         {220, 0, 0, 255}},
        6000);
    return run_case(adapter,
                    "split layout track order",
                    12,
                    4,
                    make_snapshot({left, right}, 12, 4, split),
                    {{3, 1, {0, 0, 220, 255}},
                     {8, 1, {220, 0, 0, 255}}},
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
    return run_all(adapter.Get()) ? 0 : 1;
}
