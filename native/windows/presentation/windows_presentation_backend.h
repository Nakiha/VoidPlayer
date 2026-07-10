#pragma once

#include "renderer/render/presentation_backend.h"

#include <cstdint>
#include <memory>

namespace vr {

enum class WindowsNativeOutputMode : uint8_t {
  SdrBgra8,
  HdrScRgb16Float,
};

struct WindowsPresentationBackendConfig {
  void* window_handle = nullptr;
  void* flutter_surface_export = nullptr;
  WindowsNativeOutputMode output_mode = WindowsNativeOutputMode::SdrBgra8;
};

// Rebuilt D3D11/D3D12 backends enter through this factory. The current branch
// deliberately returns null so Windows presentation fails closed.
std::unique_ptr<PresentationBackend> create_windows_presentation_backend(
    const WindowsPresentationBackendConfig& config);

}  // namespace vr
