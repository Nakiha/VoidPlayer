#include "windows/presentation/windows_presentation_backend.h"

namespace vr {

std::unique_ptr<PresentationBackend> create_windows_presentation_backend(
    const WindowsPresentationBackendConfig& config) {
  (void)config;
  return nullptr;
}

}  // namespace vr
