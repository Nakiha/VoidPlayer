#include "macos/metal_presentation_backend.h"

#include <iostream>

namespace {

int fail(const char* message) {
  std::cerr << message << "\n";
  return 1;
}

}  // namespace

int main() {
  vp_macos::MetalPresentationBackend backend;
  vr::PresentationBackendConfig config;
  config.width = 640;
  config.height = 360;
  config.headless = true;

  if (!backend.initialize(config)) {
    return fail("Metal presentation backend did not initialize");
  }
  if (backend.kind() != vr::PresentationBackendKind::Metal) {
    return fail("Metal presentation backend kind mismatch");
  }
  if (!backend.headless()) {
    return fail("Metal presentation backend should be headless");
  }
  if (backend.width() != 640 || backend.height() != 360) {
    return fail("Metal presentation backend dimensions mismatch");
  }
  if (!backend.available() || !backend.uploader()) {
    return fail("Metal presentation backend uploader is unavailable");
  }
  backend.shutdown();
  if (backend.available() || backend.uploader()) {
    return fail("Metal presentation backend did not release uploader");
  }

  VPMacOSMetalPresentationBackend* c_backend =
      VPMacOSMetalPresentationBackendCreate(640, 360);
  if (!c_backend) {
    return fail("Metal presentation backend C ABI did not initialize");
  }
  if (VPMacOSMetalPresentationBackendIsAvailable(c_backend) == 0 ||
      !VPMacOSMetalPresentationBackendUploader(c_backend)) {
    VPMacOSMetalPresentationBackendDestroy(c_backend);
    return fail("Metal presentation backend C ABI uploader is unavailable");
  }
  VPMacOSMetalPresentationBackendDestroy(c_backend);
  return 0;
}
