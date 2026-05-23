#include "video_renderer/decode/hw/hw_decode_provider.h"

#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/pixfmt.h>
}

namespace {

int fail(const char* message) {
  std::cerr << message << "\n";
  return 1;
}

}  // namespace

int main() {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    return fail("H.264 decoder is unavailable");
  }

  vr::HwDecodeInitParams params;
  params.backend = vr::RenderBackendType::Metal;
  params.device_mode = vr::DecodeDeviceMode::FfmpegOwnedHwDownloadDevice;
  params.width = 320;
  params.height = 180;

  auto result = vr::try_hw_decode_providers(codec, params);
  if (!result.success) {
    return fail("VideoToolbox provider did not initialize");
  }
  if (result.type != vr::HwDecodeType::VideoToolbox) {
    return fail("hardware provider is not VideoToolbox");
  }
  if (result.hw_pix_fmt != AV_PIX_FMT_VIDEOTOOLBOX) {
    return fail("hardware pixel format is not AV_PIX_FMT_VIDEOTOOLBOX");
  }
  if (!result.hw_device_ctx) {
    return fail("hardware device context is null");
  }

  av_buffer_unref(&result.hw_device_ctx);
  result.provider->shutdown();
  return 0;
}
