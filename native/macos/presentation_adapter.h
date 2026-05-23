#pragma once

#include "native_player_bridge.h"

#include <cstddef>
#include <cstdint>

namespace vr {
struct TextureFrame;
enum class FrameStorageKind;
}

namespace vp_macos {

enum class PresentationAdapterStatus {
  Ok,
  InvalidDestination,
  UnsupportedStorage,
  InvalidStorage,
  AllocationFailed,
};

const char* presentation_adapter_name();
const char* presentation_adapter_status_message(PresentationAdapterStatus status);
bool presentation_adapter_supports_storage(vr::FrameStorageKind kind);

PresentationAdapterStatus copy_texture_frame_to_bgra_destination_checked(
    const vr::TextureFrame& frame,
    uint8_t* dst,
    size_t dst_size,
    int32_t width,
    int32_t height,
    int32_t stride_bytes,
    VPMacOSNativeFrameInfo* out);

bool copy_texture_frame_to_bgra_destination(const vr::TextureFrame& frame,
                                            uint8_t* dst,
                                            size_t dst_size,
                                            int32_t width,
                                            int32_t height,
                                            int32_t stride_bytes,
                                            VPMacOSNativeFrameInfo* out);

bool copy_texture_frame_to_owned_bgra(const vr::TextureFrame& frame,
                                      VPMacOSNativeFrame* out);

void free_owned_bgra_frame(VPMacOSNativeFrame* frame);

}  // namespace vp_macos
