#include "macos/native_player_bridge.h"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <type_traits>

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

template <typename T>
bool require_c_abi_struct(const char* name) {
  if (!std::is_standard_layout<T>::value) {
    std::fprintf(stderr, "%s is not standard-layout\n", name);
    return false;
  }
  if (!std::is_trivial<T>::value) {
    std::fprintf(stderr, "%s is not trivial\n", name);
    return false;
  }
  return true;
}

}  // namespace

int main() {
  static_assert(VP_MACOS_NATIVE_API_VERSION == 1u,
                "bump this smoke when the macOS native ABI version changes");
  static_assert(offsetof(VPMacOSNativeFrameInfo, struct_size) == 0,
                "versioned ABI structs must start with struct_size");
  static_assert(offsetof(VPMacOSNativeFrameInfo, api_version) ==
                    sizeof(uint32_t),
                "api_version must follow struct_size");
  static_assert(VPMacOSNativeStatusOk == 0);
  static_assert(VPMacOSNativeStatusInvalidArgument < 0);
  static_assert(VPMacOSNativeStatusRendererFailed < 0);

  if (!require_c_abi_struct<VPMacOSNativeFrameInfo>("VPMacOSNativeFrameInfo") ||
      !require_c_abi_struct<VPMacOSNativeTrackInfo>("VPMacOSNativeTrackInfo") ||
      !require_c_abi_struct<VPMacOSCaptureMetrics>("VPMacOSCaptureMetrics") ||
      !require_c_abi_struct<VPMacOSNativeLayoutState>(
          "VPMacOSNativeLayoutState") ||
      !require_c_abi_struct<VPMacOSNativeLayoutPresentationParams>(
          "VPMacOSNativeLayoutPresentationParams") ||
      !require_c_abi_struct<VPMacOSNativePresentationSchedulerStats>(
          "VPMacOSNativePresentationSchedulerStats") ||
      !require_c_abi_struct<VPMacOSNativeRendererOwnedPresentationState>(
          "VPMacOSNativeRendererOwnedPresentationState") ||
      !require_c_abi_struct<VPMacOSNativeTrackDiagnosticInfo>(
          "VPMacOSNativeTrackDiagnosticInfo") ||
      !require_c_abi_struct<VPMacOSNativePlayerPerfStats>(
          "VPMacOSNativePlayerPerfStats") ||
      !require_c_abi_struct<VPMacOSNativeAudioDiagnostics>(
          "VPMacOSNativeAudioDiagnostics")) {
    return 1;
  }

  VPMacOSNativeFrameInfo frame_info{};
  VPMacOSNativeFrameInfoInit(&frame_info);
  if (frame_info.struct_size != sizeof(VPMacOSNativeFrameInfo)) {
    return fail("VPMacOSNativeFrameInfoInit wrote an unexpected struct_size");
  }
  if (frame_info.api_version != VP_MACOS_NATIVE_API_VERSION) {
    return fail("VPMacOSNativeFrameInfoInit wrote an unexpected api_version");
  }

  VPMacOSNativePlayerDestroy(nullptr);
  VPMacOSNativePlayerClose(nullptr);
  VPMacOSNativePlayerSetFrameAvailableCallback(nullptr, nullptr, nullptr);
  VPMacOSNativePlayerClearMetalPresentationTarget(nullptr);

  VPMacOSNativePlayer* player = VPMacOSNativePlayerCreate();
  if (!player) {
    return fail("VPMacOSNativePlayerCreate returned null");
  }
  VPMacOSNativePlayerSetFrameAvailableCallback(player, nullptr, nullptr);
  VPMacOSNativePlayerClearMetalPresentationTarget(player);
  VPMacOSNativePlayerDestroy(player);
  return 0;
}
