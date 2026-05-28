#pragma once

#include "metal_presentation_backend_bridge.h"
#include "video_renderer/render/presentation_backend.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vp_macos {

class MetalPresentationBackend final : public vr::PresentationBackend {
public:
  MetalPresentationBackend() = default;
  ~MetalPresentationBackend() override;

  MetalPresentationBackend(const MetalPresentationBackend&) = delete;
  MetalPresentationBackend& operator=(const MetalPresentationBackend&) = delete;

  vr::PresentationBackendKind kind() const override { return vr::PresentationBackendKind::Metal; }
  const char* name() const override { return "metal-cvpixelbuffer"; }
  bool initialize(const vr::PresentationBackendConfig& config) override;
  void shutdown() override;
  bool headless() const override { return headless_; }
  bool update_headless_output(void* output,
                              int width,
                              int height,
                              int max_track_slots) override;
  void clear_headless_output() override;
  vr::PresentationBackendStats presentation_stats() const override;
  bool copy_last_frame_info(vr::PresentationBackendFrameInfo* out) const override;
  bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) override;
  const char* last_error() const override { return last_error_.c_str(); }
  bool draw_frame(const vr::RendererDrawSnapshot& snapshot,
                  const vr::PresentationBackendDrawHooks& hooks) override;

  bool available() const;
  int width() const { return width_; }
  int height() const { return height_; }
  VPMacOSMetalUploader* uploader() const { return uploader_; }
  void set_draw_target(void* pixel_buffer,
                       int32_t width,
                       int32_t height,
                       int32_t max_track_slots);
  void clear_draw_target();
  bool copy_last_draw_frame_info(VPMacOSNativeFrameInfo* out) const;

private:
  void set_last_error(std::string error);
  void mark_draw_failure(std::string error);
  void mark_draw_success(const VPMacOSNativeFrameInfo& frame_info);
  void record_overlay_result(bool expected,
                             bool applied,
                             bool gpu_attempted,
                             bool gpu_succeeded,
                             bool cpu_attempted,
                             size_t line_rect_count);

  VPMacOSMetalUploader* uploader_ = nullptr;
  void* draw_target_pixel_buffer_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int draw_target_width_ = 0;
  int draw_target_height_ = 0;
  int draw_target_max_track_slots_ = VPMacOSNativeMaxTracks;
  bool last_draw_frame_info_available_ = false;
  VPMacOSNativeFrameInfo last_draw_frame_info_ = {};
  std::string last_error_;
  uint64_t draw_failure_count_ = 0;
  uint64_t consecutive_draw_failures_ = 0;
  uint64_t staging_allocation_count_ = 0;
  uint64_t staging_reuse_count_ = 0;
  uint64_t draw_profiler_count_ = 0;
  size_t staging_max_bytes_ = 0;
  bool overlay_last_expected_ = false;
  bool overlay_last_applied_ = false;
  uint64_t overlay_last_line_rect_count_ = 0;
  uint64_t overlay_expected_count_ = 0;
  uint64_t overlay_applied_count_ = 0;
  uint64_t overlay_missed_count_ = 0;
  uint64_t overlay_gpu_success_count_ = 0;
  uint64_t overlay_gpu_failure_count_ = 0;
  uint64_t overlay_cpu_fallback_count_ = 0;
  std::vector<uint8_t> staging_buffer_;
  bool last_draw_succeeded_ = false;
  bool headless_ = true;
};

}  // namespace vp_macos

struct VPMacOSMetalPresentationBackend {
  vp_macos::MetalPresentationBackend impl;
};
