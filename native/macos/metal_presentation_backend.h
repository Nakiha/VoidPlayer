#pragma once

#include "native_player_bridge.h"
#include "video_renderer/render/presentation_backend.h"

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

private:
  VPMacOSMetalUploader* uploader_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  bool headless_ = true;
};

}  // namespace vp_macos
