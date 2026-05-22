#include "video_renderer/decode/software_frame_packer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

bool expect_byte(const std::vector<uint8_t>& data, size_t index, uint8_t value) {
  return index < data.size() && data[index] == value;
}

int check_odd_nv21_packs_to_even_nv12_layout() {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return fail("failed to allocate nv21 frame");
  }

  std::array<uint8_t, 9> y = {
      10, 20, 30,
      40, 50, 60,
      70, 80, 90,
  };
  std::array<uint8_t, 8> vu = {
      200, 100, 210, 110,
      220, 120, 230, 130,
  };

  frame->format = AV_PIX_FMT_NV21;
  frame->width = 3;
  frame->height = 3;
  frame->data[0] = y.data();
  frame->data[1] = vu.data();
  frame->linesize[0] = 3;
  frame->linesize[1] = 4;

  vr::TextureFrame result;
  const bool converted = vr::convert_frame_to_cpu_nv12(
      frame,
      "software-frame-packer-smoke",
      result);
  av_frame_free(&frame);
  if (!converted) {
    return fail("failed to convert odd nv21 frame");
  }
  const auto* storage = result.cpu_nv12_storage();
  if (!storage || !storage->data || !result.is_nv12 || result.is_p010 ||
      result.width != 3 || result.height != 3 ||
      storage->y_stride != 4 || storage->uv_stride != 4 ||
      storage->coded_width != 4 || storage->coded_height != 4 ||
      storage->data->size() != 24) {
    return fail("unexpected odd nv21 packed layout metadata");
  }

  const auto& packed = *storage->data;
  const std::array<uint8_t, 24> expected = {
      10, 20, 30, 30,
      40, 50, 60, 60,
      70, 80, 90, 90,
      70, 80, 90, 90,
      100, 200, 110, 210,
      120, 220, 130, 230,
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    if (!expect_byte(packed, i, expected[i])) {
      return fail("unexpected odd nv21 packed byte");
    }
  }
  return 0;
}

int check_planar_yuv420_wrap_preserves_source_layout() {
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    return fail("failed to allocate yuv420p frame");
  }

  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = 3;
  frame->height = 3;
  frame->buf[0] = av_buffer_alloc(12);
  frame->buf[1] = av_buffer_alloc(6);
  frame->buf[2] = av_buffer_alloc(6);
  if (!frame->buf[0] || !frame->buf[1] || !frame->buf[2]) {
    av_frame_free(&frame);
    return fail("failed to allocate yuv420p frame buffers");
  }
  frame->data[0] = frame->buf[0]->data;
  frame->data[1] = frame->buf[1]->data;
  frame->data[2] = frame->buf[2]->data;
  frame->linesize[0] = 4;
  frame->linesize[1] = 3;
  frame->linesize[2] = 3;

  vr::TextureFrame result;
  const bool wrapped = vr::wrap_frame_as_cpu_planar_yuv420(frame, result);
  av_frame_free(&frame);
  if (!wrapped) {
    return fail("failed to wrap planar yuv420p frame");
  }
  const auto* storage = result.cpu_planar_yuv_storage();
  if (!storage || !storage->frame_ref || result.is_nv12 || result.is_p010 ||
      result.width != 3 || result.height != 3 ||
      storage->strides[0] != 4 || storage->strides[1] != 3 || storage->strides[2] != 3 ||
      storage->plane_widths[0] != 3 || storage->plane_widths[1] != 2 ||
      storage->plane_widths[2] != 2 ||
      storage->plane_heights[0] != 3 || storage->plane_heights[1] != 2 ||
      storage->plane_heights[2] != 2 ||
      storage->bytes_per_sample != 1) {
    return fail("unexpected planar yuv420p wrap metadata");
  }
  return 0;
}

}  // namespace

int main() {
  if (const int ret = check_odd_nv21_packs_to_even_nv12_layout(); ret != 0) {
    return ret;
  }
  if (const int ret = check_planar_yuv420_wrap_preserves_source_layout(); ret != 0) {
    return ret;
  }
  return 0;
}
