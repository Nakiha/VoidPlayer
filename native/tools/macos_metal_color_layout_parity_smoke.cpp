#include "macos/metal/metal_presentation_backend.h"

#include "renderer/frame/frame_storage.h"
#include "renderer/render/renderer_draw_snapshot.h"
#include "renderer/render/presentation_snapshot.h"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

struct SampleExpectation {
  int x = 0;
  int y = 0;
  Bgra expected;
};

struct PlanarStorageOwner {
  std::vector<uint8_t> y;
  std::vector<uint8_t> u;
  std::vector<uint8_t> v;
};

class ScopedPixelBuffer {
public:
  ScopedPixelBuffer(int width, int height) {
    CFDictionaryRef io_surface_properties = CFDictionaryCreate(
        kCFAllocatorDefault,
        nullptr,
        nullptr,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    const void* keys[] = {
        kCVPixelBufferMetalCompatibilityKey,
        kCVPixelBufferIOSurfacePropertiesKey,
    };
    const void* values[] = {
        kCFBooleanTrue,
        io_surface_properties,
    };
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault,
        keys,
        values,
        2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CVPixelBufferCreate(kCFAllocatorDefault,
                        width,
                        height,
                        kCVPixelFormatType_32BGRA,
                        attrs,
                        &buffer_);
    if (attrs) {
      CFRelease(attrs);
    }
    if (io_surface_properties) {
      CFRelease(io_surface_properties);
    }
  }

  ~ScopedPixelBuffer() {
    if (buffer_) {
      CVPixelBufferRelease(buffer_);
    }
  }

  ScopedPixelBuffer(const ScopedPixelBuffer&) = delete;
  ScopedPixelBuffer& operator=(const ScopedPixelBuffer&) = delete;

  CVPixelBufferRef get() const { return buffer_; }

private:
  CVPixelBufferRef buffer_ = nullptr;
};

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
  r = std::clamp(r - kSdrBias, 0.0, 1.0);
  g = std::clamp(g - kSdrBias, 0.0, 1.0);
  b = std::clamp(b - kSdrBias, 0.0, 1.0);
  return Bgra{clamp_u8(b), clamp_u8(g), clamp_u8(r), 255};
}

Bgra read_bgra(const std::vector<uint8_t>& bgra, int width, int x, int y) {
  const size_t offset =
      (static_cast<size_t>(y) * static_cast<size_t>(width) +
       static_cast<size_t>(x)) *
      4u;
  return Bgra{bgra[offset + 0], bgra[offset + 1], bgra[offset + 2], bgra[offset + 3]};
}

int channel_diff(uint8_t actual, uint8_t expected) {
  return std::abs(static_cast<int>(actual) - static_cast<int>(expected));
}

bool expect_pixels(const char* case_name,
                   const char* storage_kind,
                   int range,
                   int matrix,
                   int width,
                   int height,
                   const std::vector<uint8_t>& bgra,
                   int capture_width,
                   const std::vector<SampleExpectation>& samples,
                   int tolerance) {
  int max_diff = 0;
  int diff_total = 0;
  int channel_count = 0;
  for (const auto& sample : samples) {
    const Bgra actual = read_bgra(bgra, capture_width, sample.x, sample.y);
    const int diffs[] = {
        channel_diff(actual.b, sample.expected.b),
        channel_diff(actual.g, sample.expected.g),
        channel_diff(actual.r, sample.expected.r),
        channel_diff(actual.a, sample.expected.a),
    };
    for (int diff : diffs) {
      max_diff = std::max(max_diff, diff);
      diff_total += diff;
      ++channel_count;
    }
    if (diffs[0] > tolerance || diffs[1] > tolerance ||
        diffs[2] > tolerance || diffs[3] > tolerance) {
      const double avg_diff = channel_count > 0
          ? static_cast<double>(diff_total) / static_cast<double>(channel_count)
          : 0.0;
      std::fprintf(stderr,
                   "%s failed at (%d,%d): storage=%s range=%d matrix=%d "
                   "dims=%dx%d actual BGRA=(%u,%u,%u,%u) expected=(%u,%u,%u,%u) "
                   "max_diff=%d avg_diff=%.2f tolerance=%d\n",
                   case_name,
                   sample.x,
                   sample.y,
                   storage_kind,
                   range,
                   matrix,
                   width,
                   height,
                   static_cast<unsigned>(actual.b),
                   static_cast<unsigned>(actual.g),
                   static_cast<unsigned>(actual.r),
                   static_cast<unsigned>(actual.a),
                   static_cast<unsigned>(sample.expected.b),
                   static_cast<unsigned>(sample.expected.g),
                   static_cast<unsigned>(sample.expected.r),
                   static_cast<unsigned>(sample.expected.a),
                   max_diff,
                   avg_diff,
                   tolerance);
      if (width <= 16 && height <= 8) {
        for (int row_y = 0; row_y < height; ++row_y) {
          std::fprintf(stderr, "%s row %d:", case_name, row_y);
          for (int col_x = 0; col_x < width; ++col_x) {
            const Bgra p = read_bgra(bgra, capture_width, col_x, row_y);
            std::fprintf(stderr,
                         " (%u,%u,%u)",
                         static_cast<unsigned>(p.b),
                         static_cast<unsigned>(p.g),
                         static_cast<unsigned>(p.r));
          }
          std::fprintf(stderr, "\n");
        }
      }
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
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4u,
      0);
  size_t i = 0;
  for (const auto& pixel : pixels) {
    if (i >= static_cast<size_t>(width) * static_cast<size_t>(height)) {
      break;
    }
    (*data)[i * 4u + 0u] = pixel.b;
    (*data)[i * 4u + 1u] = pixel.g;
    (*data)[i * 4u + 2u] = pixel.r;
    (*data)[i * 4u + 3u] = pixel.a;
    ++i;
  }
  vr::TextureFrame frame;
  frame.width = width;
  frame.height = height;
  frame.pts_us = pts_us;
  frame.duration_us = 16667;
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
      static_cast<size_t>(y_stride) * static_cast<size_t>(coded_height) +
          static_cast<size_t>(uv_stride) * static_cast<size_t>((coded_height + 1) / 2),
      0);
  for (int y = 0; y < coded_height; ++y) {
    std::fill_n(data->data() + static_cast<size_t>(y) * y_stride,
                coded_width,
                y_value);
  }
  for (int y = 0; y < (coded_height + 1) / 2; ++y) {
    uint8_t* row = data->data() + static_cast<size_t>(y_stride) * coded_height +
        static_cast<size_t>(y) * uv_stride;
    for (int x = 0; x < (coded_width + 1) / 2; ++x) {
      row[x * 2 + 0] = u_value;
      row[x * 2 + 1] = v_value;
    }
  }
  vr::TextureFrame frame;
  frame.width = width;
  frame.height = height;
  frame.pts_us = pts_us;
  frame.duration_us = 16667;
  frame.is_nv12 = true;
  frame.color = {range, matrix, vr::VIDEO_COLOR_TRANSFER_SDR,
                 vr::default_presentation_color_primaries_for_matrix(matrix)};
  frame.storage =
      vr::CpuNv12FrameStorage{data, y_stride, uv_stride, false, coded_width, coded_height};
  return frame;
}

vr::TextureFrame make_p010_frame(int width,
                                 int height,
                                 uint16_t y10,
                                 uint16_t u10,
                                 uint16_t v10,
                                 int range,
                                 int matrix,
                                 int64_t pts_us) {
  const int coded_width = width;
  const int coded_height = height;
  const int y_stride = coded_width * 2;
  const int uv_stride = coded_width * 2;
  auto data = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(y_stride) * static_cast<size_t>(coded_height) +
          static_cast<size_t>(uv_stride) * static_cast<size_t>(coded_height / 2),
      0);
  auto write10 = [](uint8_t* dst, uint16_t value10) {
    const uint16_t packed = static_cast<uint16_t>(value10 << 6);
    dst[0] = static_cast<uint8_t>(packed & 0xffu);
    dst[1] = static_cast<uint8_t>(packed >> 8);
  };
  for (int y = 0; y < coded_height; ++y) {
    uint8_t* row = data->data() + static_cast<size_t>(y) * y_stride;
    for (int x = 0; x < coded_width; ++x) {
      write10(row + x * 2, y10);
    }
  }
  uint8_t* uv = data->data() + static_cast<size_t>(y_stride) * coded_height;
  for (int y = 0; y < coded_height / 2; ++y) {
    uint8_t* row = uv + static_cast<size_t>(y) * uv_stride;
    for (int x = 0; x < coded_width / 2; ++x) {
      write10(row + x * 4 + 0, u10);
      write10(row + x * 4 + 2, v10);
    }
  }
  vr::TextureFrame frame;
  frame.width = width;
  frame.height = height;
  frame.pts_us = pts_us;
  frame.duration_us = 16667;
  frame.is_nv12 = true;
  frame.is_p010 = true;
  frame.color = {range, matrix, vr::VIDEO_COLOR_TRANSFER_SDR,
                 vr::default_presentation_color_primaries_for_matrix(matrix)};
  frame.storage =
      vr::CpuNv12FrameStorage{data, y_stride, uv_stride, true, coded_width, coded_height};
  return frame;
}

vr::TextureFrame make_planar_yuv420_frame(int width,
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
  auto owner = std::make_shared<PlanarStorageOwner>();
  const int chroma_width = (coded_width + 1) / 2;
  const int chroma_height = (coded_height + 1) / 2;
  owner->y.assign(static_cast<size_t>(y_stride) * coded_height, 0);
  owner->u.assign(static_cast<size_t>(uv_stride) * chroma_height, 0);
  owner->v.assign(static_cast<size_t>(uv_stride) * chroma_height, 0);
  for (int y = 0; y < coded_height; ++y) {
    std::fill_n(owner->y.data() + static_cast<size_t>(y) * y_stride,
                coded_width,
                y_value);
  }
  for (int y = 0; y < chroma_height; ++y) {
    std::fill_n(owner->u.data() + static_cast<size_t>(y) * uv_stride,
                chroma_width,
                u_value);
    std::fill_n(owner->v.data() + static_cast<size_t>(y) * uv_stride,
                chroma_width,
                v_value);
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
  storage.plane_widths[1] = chroma_width;
  storage.plane_widths[2] = chroma_width;
  storage.plane_heights[0] = coded_height;
  storage.plane_heights[1] = chroma_height;
  storage.plane_heights[2] = chroma_height;
  storage.bytes_per_sample = 1;

  vr::TextureFrame frame;
  frame.width = width;
  frame.height = height;
  frame.pts_us = pts_us;
  frame.duration_us = 16667;
  frame.color = {range, matrix, vr::VIDEO_COLOR_TRANSFER_SDR,
                 vr::default_presentation_color_primaries_for_matrix(matrix)};
  frame.storage = storage;
  return frame;
}

vr::RendererDrawSnapshot make_snapshot(const std::vector<vr::TextureFrame>& frames,
                                        int target_width,
                                        int target_height,
                                        const vr::LayoutState& layout) {
  vr::RendererDrawSnapshot snapshot;
  snapshot.decision.should_present = true;
  snapshot.decision.current_pts_us = frames.empty() ? 0 : frames.front().pts_us;
  snapshot.layout = layout;
  snapshot.target_width = target_width;
  snapshot.target_height = target_height;
  snapshot.background_color[3] = 1.0f;
  for (size_t slot = 0; slot < frames.size() && slot < vr::kMaxTracks; ++slot) {
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
        frame.height > 0 ? static_cast<float>(frame.width) / frame.height : 1.0f;
    snapshot.track_geometry[slot] = {
        true,
        frame.width,
        frame.height,
        snapshot.tracks[slot].video_aspect,
    };
  }
  return snapshot;
}

bool draw_capture(vp_macos::MetalPresentationBackend& backend,
                  const vr::RendererDrawSnapshot& snapshot,
                  std::vector<uint8_t>& bgra,
                  int& width,
                  int& height,
                  const char* case_name) {
  vr::PresentationBackendDrawHooks hooks;
  if (!backend.draw_frame(snapshot, hooks)) {
    std::fprintf(stderr,
                 "%s draw_frame failed: %s\n",
                 case_name,
                 backend.last_error());
    return false;
  }
  if (!backend.capture_front_buffer(bgra, width, height)) {
    std::fprintf(stderr, "%s capture_front_buffer failed\n", case_name);
    return false;
  }
  return true;
}

bool run_case(const char* case_name,
              const char* storage_kind,
              int range,
              int matrix,
              int target_width,
              int target_height,
              const vr::RendererDrawSnapshot& snapshot,
              const std::vector<SampleExpectation>& samples,
              int tolerance = 4) {
  ScopedPixelBuffer pixel_buffer(target_width, target_height);
  if (!pixel_buffer.get()) {
    std::fprintf(stderr, "%s could not create CVPixelBuffer target\n", case_name);
    return false;
  }
  vp_macos::MetalPresentationBackend backend;
  vr::PresentationBackendConfig config;
  config.output = pixel_buffer.get();
  config.width = target_width;
  config.height = target_height;
  config.max_track_slots = static_cast<int>(vr::kMaxTracks);
  config.headless = true;
  if (!backend.initialize(config)) {
    std::fprintf(stderr, "%s could not initialize Metal backend\n", case_name);
    return false;
  }
  std::vector<uint8_t> bgra;
  int capture_width = 0;
  int capture_height = 0;
  if (!draw_capture(backend, snapshot, bgra, capture_width, capture_height, case_name)) {
    return false;
  }
  if (capture_width != target_width || capture_height != target_height) {
    std::fprintf(stderr,
                 "%s capture dimensions mismatch: got %dx%d expected %dx%d\n",
                 case_name,
                 capture_width,
                 capture_height,
                 target_width,
                 target_height);
    return false;
  }
  const bool ok = expect_pixels(case_name,
                                storage_kind,
                                range,
                                matrix,
                                target_width,
                                target_height,
                                bgra,
                                capture_width,
                                samples,
                                tolerance);
  backend.shutdown();
  return ok;
}

bool run_bgra_channel_order_case() {
  vr::LayoutState layout;
  layout.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
  const auto frame = make_bgra_frame(
      2,
      2,
      {
          {10, 20, 200, 255},
          {30, 180, 40, 255},
          {220, 50, 60, 255},
          {70, 80, 90, 255},
      },
      1000);
  const auto snapshot = make_snapshot({frame}, 2, 2, layout);
  return run_case("BGRA channel/order",
                  "bgra",
                  vr::VIDEO_COLOR_RANGE_FULL,
                  vr::VIDEO_COLOR_MATRIX_BT709,
                  2,
                  2,
                  snapshot,
                  {
                      {0, 0, {10, 20, 200, 255}},
                      {1, 0, {30, 180, 40, 255}},
                      {0, 1, {220, 50, 60, 255}},
                      {1, 1, {70, 80, 90, 255}},
                  },
                  0);
}

bool run_nv12_limited_bt709_odd_stride_case() {
  constexpr uint8_t y = 180;
  constexpr uint8_t u = 90;
  constexpr uint8_t v = 200;
  const Bgra expected = reference_yuv_to_bgra(
      y / 255.0,
      u / 255.0,
      v / 255.0,
      vr::VIDEO_COLOR_RANGE_LIMITED,
      vr::VIDEO_COLOR_MATRIX_BT709);
  vr::LayoutState layout;
  layout.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
  const auto frame = make_nv12_frame(5,
                                     3,
                                     6,
                                     4,
                                     8,
                                     8,
                                     y,
                                     u,
                                     v,
                                     vr::VIDEO_COLOR_RANGE_LIMITED,
                                     vr::VIDEO_COLOR_MATRIX_BT709,
                                     2000);
  const auto snapshot = make_snapshot({frame}, 5, 3, layout);
  return run_case("NV12 limited BT.709 odd dimensions/padded stride",
                  "nv12",
                  vr::VIDEO_COLOR_RANGE_LIMITED,
                  vr::VIDEO_COLOR_MATRIX_BT709,
                  5,
                  3,
                  snapshot,
                  {
                      {0, 0, expected},
                      {4, 2, expected},
                  });
}

bool run_planar_yuv420_case(const char* case_name,
                            int range,
                            int matrix,
                            uint8_t y,
                            uint8_t u,
                            uint8_t v) {
  const Bgra expected = reference_yuv_to_bgra(
      y / 255.0,
      u / 255.0,
      v / 255.0,
      range,
      matrix);
  vr::LayoutState layout;
  layout.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
  const auto frame = make_planar_yuv420_frame(5,
                                              3,
                                              6,
                                              4,
                                              8,
                                              4,
                                              y,
                                              u,
                                              v,
                                              range,
                                              matrix,
                                              3000);
  const auto snapshot = make_snapshot({frame}, 5, 3, layout);
  return run_case(case_name,
                  "yuv420p",
                  range,
                  matrix,
                  5,
                  3,
                  snapshot,
                  {
                      {1, 1, expected},
                      {4, 2, expected},
                  });
}

bool run_p010_high_bit_case() {
  constexpr uint16_t y = 640;
  constexpr uint16_t u = 384;
  constexpr uint16_t v = 768;
  const Bgra expected = reference_yuv_to_bgra(
      y / 1023.0,
      u / 1023.0,
      v / 1023.0,
      vr::VIDEO_COLOR_RANGE_LIMITED,
      vr::VIDEO_COLOR_MATRIX_BT709);
  vr::LayoutState layout;
  layout.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
  const auto frame = make_p010_frame(4,
                                     4,
                                     y,
                                     u,
                                     v,
                                     vr::VIDEO_COLOR_RANGE_LIMITED,
                                     vr::VIDEO_COLOR_MATRIX_BT709,
                                     4000);
  const auto snapshot = make_snapshot({frame}, 4, 4, layout);
  return run_case("P010 high-bit limited BT.709",
                  "p010",
                  vr::VIDEO_COLOR_RANGE_LIMITED,
                  vr::VIDEO_COLOR_MATRIX_BT709,
                  4,
                  4,
                  snapshot,
                  {
                      {1, 1, expected},
                      {3, 3, expected},
                  },
                  5);
}

bool run_aspect_fit_case() {
  vr::LayoutState layout;
  layout.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
  const auto frame = make_bgra_frame(
      2,
      4,
      {
          {0, 0, 220, 255}, {0, 0, 220, 255},
          {0, 0, 220, 255}, {0, 0, 220, 255},
          {0, 0, 220, 255}, {0, 0, 220, 255},
          {0, 0, 220, 255}, {0, 0, 220, 255},
      },
      5000);
  const auto snapshot = make_snapshot({frame}, 8, 4, layout);
  auto themed_snapshot = snapshot;
  themed_snapshot.background_color[0] = 24.0f / 255.0f;
  themed_snapshot.background_color[1] = 32.0f / 255.0f;
  themed_snapshot.background_color[2] = 40.0f / 255.0f;
  themed_snapshot.background_color[3] = 1.0f;
  return run_case("layout aspect fit themed bars",
                  "bgra",
                  vr::VIDEO_COLOR_RANGE_FULL,
                  vr::VIDEO_COLOR_MATRIX_BT709,
                  8,
                  4,
                  themed_snapshot,
                  {
                      {0, 1, {40, 32, 24, 255}},
                      {3, 1, {0, 0, 220, 255}},
                      {4, 2, {0, 0, 220, 255}},
                      {7, 1, {40, 32, 24, 255}},
                  },
                  1);
}

bool run_split_layout_case() {
  vr::LayoutState layout;
  layout.mode = vr::LAYOUT_SPLIT_SCREEN;
  layout.split_pos = 0.5f;
  layout.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
  layout.order[0] = 0;
  layout.order[1] = 1;
  const auto left = make_bgra_frame(
      3,
      2,
      {
          {0, 0, 220, 255}, {0, 0, 220, 255}, {0, 0, 220, 255},
          {0, 0, 220, 255}, {0, 0, 220, 255}, {0, 0, 220, 255},
      },
      6000);
  const auto right = make_bgra_frame(
      3,
      2,
      {
          {220, 0, 0, 255}, {220, 0, 0, 255}, {220, 0, 0, 255},
          {220, 0, 0, 255}, {220, 0, 0, 255}, {220, 0, 0, 255},
      },
      6000);
  const auto snapshot = make_snapshot({left, right}, 12, 4, layout);
  return run_case("split layout track order",
                  "bgra",
                  vr::VIDEO_COLOR_RANGE_FULL,
                  vr::VIDEO_COLOR_MATRIX_BT709,
                  12,
                  4,
                  snapshot,
                  {
                      {3, 1, {0, 0, 220, 255}},
                      {8, 1, {220, 0, 0, 255}},
                  },
                  1);
}

}  // namespace

int main() {
  if (!run_bgra_channel_order_case()) {
    return 1;
  }
  if (!run_nv12_limited_bt709_odd_stride_case()) {
    return 1;
  }
  if (!run_planar_yuv420_case("YUV420P full BT.601",
                              vr::VIDEO_COLOR_RANGE_FULL,
                              vr::VIDEO_COLOR_MATRIX_BT601,
                              170,
                              100,
                              190)) {
    return 1;
  }
  if (!run_planar_yuv420_case("YUV420P limited BT.709",
                              vr::VIDEO_COLOR_RANGE_LIMITED,
                              vr::VIDEO_COLOR_MATRIX_BT709,
                              150,
                              90,
                              210)) {
    return 1;
  }
  if (!run_p010_high_bit_case()) {
    return 1;
  }
  if (!run_aspect_fit_case()) {
    return 1;
  }
  if (!run_split_layout_case()) {
    return 1;
  }
  return 0;
}
