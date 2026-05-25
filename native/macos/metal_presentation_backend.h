#pragma once

#include "native_player_bridge.h"
#include "video_renderer/render/presentation_backend.h"

#include <cstddef>
#include <cstdint>
#include <string>

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
  const std::string& last_error() const { return last_error_; }

private:
  void set_last_error(std::string error);

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
  bool headless_ = true;
};

}  // namespace vp_macos

struct VPMacOSMetalPresentationBackend {
  vp_macos::MetalPresentationBackend impl;
};
