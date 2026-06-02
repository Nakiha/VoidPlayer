#include "renderer/render/presentation_snapshot.h"
#include "renderer/render/presentation_package.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

vr::TextureFrame make_cpu_rgba_frame() {
  auto data = std::make_shared<std::vector<uint8_t>>(4 * 640 * 360, 0x80);
  vr::TextureFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.pts_us = 1000;
  frame.dts_us = 900;
  frame.duration_us = 33366;
  frame.storage = vr::CpuRgbaFrameStorage{data, 640 * 4};
  return frame;
}

vr::TextureFrame make_odd_nv12_frame() {
  auto data = std::make_shared<std::vector<uint8_t>>(8 * 4 + 8 * 2, 0x80);
  vr::TextureFrame frame;
  frame.width = 7;
  frame.height = 3;
  frame.pts_us = 2000;
  frame.dts_us = 1900;
  frame.duration_us = 33366;
  frame.is_nv12 = true;
  frame.color.range = vr::VIDEO_COLOR_RANGE_FULL;
  frame.storage = vr::CpuNv12FrameStorage{
      data,
      8,
      8,
      false,
      8,
      4,
  };
  return frame;
}

}  // namespace

int main() {
  vr::PresentDecision decision;
  decision.should_present = true;
  decision.current_pts_us = 2100;
  decision.file_ids[0] = 10;
  decision.track_generations[0] = 100;
  decision.frames[0] = make_cpu_rgba_frame();
  decision.file_ids[1] = 20;
  decision.track_generations[1] = 200;
  decision.frames[1] = make_odd_nv12_frame();

  vr::LayoutState layout;
  layout.mode = vr::LAYOUT_SPLIT_SCREEN;
  layout.split_pos = 0.35f;
  layout.zoom_ratio = 1.5f;

  vr::LayoutTrackGeometryList geometry = {};
  geometry[0] = {true, 640, 360, 16.0f / 9.0f};
  geometry[1] = {true, 7, 3, 7.0f / 3.0f};
  float background[4] = {0.1f, 0.2f, 0.3f, 1.0f};

  const auto snapshot = vr::build_presentation_snapshot(
      decision, layout, geometry, 1920, 1080, background);

  if (!snapshot.should_present || snapshot.frame_count != 2 ||
      snapshot.current_pts_us != 2100) {
    return fail("presentation snapshot did not preserve decision identity");
  }
  if (snapshot.constants.mode != vr::LAYOUT_SPLIT_SCREEN ||
      snapshot.constants.track_count != 2 ||
      snapshot.constants.split_pos != 0.35f ||
      snapshot.constants.zoom_ratio != 1.5f ||
      snapshot.constants.background_color[2] != 0.3f) {
    return fail("presentation snapshot did not preserve layout constants");
  }
  if (!snapshot.frames[0].present ||
      snapshot.frames[0].file_id != 10 ||
      snapshot.frames[0].track_generation != 100 ||
      snapshot.frames[0].storage_kind != vr::FrameStorageKind::CpuRgba ||
      snapshot.frames[0].storage_class != vr::FrameStorageClass::CpuPixels ||
      snapshot.frames[0].color_matrix != vr::VIDEO_COLOR_MATRIX_BT601 ||
      snapshot.frames[0].color_primaries != vr::VIDEO_COLOR_PRIMARIES_BT601) {
    return fail("presentation snapshot did not preserve CPU RGBA frame metadata");
  }
  if (!snapshot.frames[1].present ||
      snapshot.frames[1].file_id != 20 ||
      snapshot.frames[1].track_generation != 200 ||
      snapshot.frames[1].storage_kind != vr::FrameStorageKind::CpuNv12 ||
      snapshot.frames[1].storage_class != vr::FrameStorageClass::CpuPixels ||
      !snapshot.frames[1].is_nv12 ||
      snapshot.frames[1].is_p010 ||
      snapshot.frames[1].y_stride != 8 ||
      snapshot.frames[1].uv_stride != 8 ||
      snapshot.frames[1].coded_width != 8 ||
      snapshot.frames[1].coded_height != 4 ||
      snapshot.frames[1].color_range != vr::VIDEO_COLOR_RANGE_FULL ||
      snapshot.frames[1].color_matrix != vr::VIDEO_COLOR_MATRIX_BT601) {
    return fail("presentation snapshot did not preserve NV12 frame metadata");
  }
  if (snapshot.frames[1].nv12_uv_scale_x != 7.0f / 8.0f ||
      snapshot.frames[1].nv12_uv_scale_y != 3.0f / 4.0f ||
      snapshot.constants.nv12_mask != (1 << 1) ||
      snapshot.constants.nv12_uv_scale_x[1] != 7.0f / 8.0f ||
      snapshot.constants.nv12_uv_scale_y[1] != 3.0f / 4.0f) {
    return fail("presentation snapshot did not preserve NV12 coded-size metadata");
  }
  const auto package_layout = vr::describe_presentation_package_layout(7, 3, 2);
  if (package_layout.bgra_row_bytes != 28 ||
      package_layout.bgra_track_stride_bytes != 84 ||
      package_layout.bgra_max_bytes != 168 ||
      package_layout.yuv_max_bytes != 192 ||
      package_layout.max_bytes != 192) {
    return fail("presentation package layout did not preserve BGRA/YUV sizing");
  }
  if (vr::describe_presentation_package_layout(0, 3, 2).max_bytes != 0) {
    return fail("presentation package layout accepted invalid dimensions");
  }
  return 0;
}
