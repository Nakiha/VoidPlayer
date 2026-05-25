#pragma once

#include "macos/native_player_types.h"

#include <cstddef>
#include <cstdint>

namespace vr {
struct TextureFrame;
enum class FrameStorageKind;
}

namespace vp_macos {

struct OwnedBGRAFrame {
  int32_t width = 0;
  int32_t height = 0;
  int64_t pts_us = 0;
  int64_t dts_us = 0;
  int64_t duration_us = 0;
  uint8_t* bgra = nullptr;
  size_t bgra_size = 0;
};

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
                                      OwnedBGRAFrame* out);

void free_owned_bgra_frame(OwnedBGRAFrame* frame);

}  // namespace vp_macos
