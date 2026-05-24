#pragma once

#include "native_player_bridge.h"
#include "video_renderer/render/presentation_backend.h"

#include <cstddef>
#include <cstdint>

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

  bool available() const;
  int width() const { return width_; }
  int height() const { return height_; }
  VPMacOSMetalUploader* uploader() const { return uploader_; }
  int copy_current_frame_with_layout(VPMacOSNativePlayer* player,
                                     void* pixel_buffer,
                                     int32_t width,
                                     int32_t height,
                                     int32_t max_track_slots,
                                     int32_t wait_timeout_ms,
                                     VPMacOSNativeFrameInfo* out,
                                     char* error,
                                     size_t error_size);

private:
  VPMacOSMetalUploader* uploader_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  bool headless_ = true;
};

}  // namespace vp_macos
