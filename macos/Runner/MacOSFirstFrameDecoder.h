#ifndef MACOS_FIRST_FRAME_DECODER_H_
#define MACOS_FIRST_FRAME_DECODER_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPMacOSDecodedFrame {
  int32_t width;
  int32_t height;
  int64_t duration_us;
  int64_t pts_us;
  uint8_t* bgra;
  size_t bgra_size;
} VPMacOSDecodedFrame;

int VPMacOSDecodeFirstVideoFrameBGRA(const char* path,
                                     VPMacOSDecodedFrame* out,
                                     char* error,
                                     size_t error_size);

void VPMacOSDecodedFrameFree(VPMacOSDecodedFrame* frame);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MACOS_FIRST_FRAME_DECODER_H_
