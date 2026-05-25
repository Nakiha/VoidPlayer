#include "macos/metal_texture_wrapping.h"

namespace vp_macos {

ScopedCVMetalTexture::~ScopedCVMetalTexture() {
  reset();
}

ScopedCVMetalTexture::ScopedCVMetalTexture(ScopedCVMetalTexture&& other) noexcept
    : texture_(other.texture_) {
  other.texture_ = nullptr;
}

ScopedCVMetalTexture& ScopedCVMetalTexture::operator=(
    ScopedCVMetalTexture&& other) noexcept {
  if (this != &other) {
    reset(other.texture_);
    other.texture_ = nullptr;
  }
  return *this;
}

void ScopedCVMetalTexture::reset(CVMetalTextureRef texture) {
  if (texture_) {
    CFRelease(texture_);
  }
  texture_ = texture;
}

id<MTLTexture> ScopedCVMetalTexture::texture() const {
  return texture_ ? CVMetalTextureGetTexture(texture_) : nil;
}

bool ScopedCVMetalTexture::valid() const {
  return texture() != nil;
}

CVReturn create_cv_metal_texture(CVMetalTextureCacheRef cache,
                                 CVPixelBufferRef pixelBuffer,
                                 MTLPixelFormat pixelFormat,
                                 size_t width,
                                 size_t height,
                                 size_t planeIndex,
                                 ScopedCVMetalTexture* out) {
  if (!out) {
    return kCVReturnInvalidArgument;
  }
  out->reset();
  CVMetalTextureRef texture = nullptr;
  const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      cache,
      pixelBuffer,
      nullptr,
      pixelFormat,
      width,
      height,
      planeIndex,
      &texture);
  if (status == kCVReturnSuccess && texture) {
    out->reset(texture);
  } else if (texture) {
    CFRelease(texture);
  }
  return status;
}

}  // namespace vp_macos
