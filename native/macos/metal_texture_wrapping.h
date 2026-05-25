#ifndef VOIDPLAYER_MACOS_METAL_TEXTURE_WRAPPING_H_
#define VOIDPLAYER_MACOS_METAL_TEXTURE_WRAPPING_H_

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>

#include <cstddef>

namespace vp_macos {

class ScopedCVMetalTexture {
 public:
  ScopedCVMetalTexture() = default;
  ~ScopedCVMetalTexture();

  ScopedCVMetalTexture(const ScopedCVMetalTexture&) = delete;
  ScopedCVMetalTexture& operator=(const ScopedCVMetalTexture&) = delete;

  ScopedCVMetalTexture(ScopedCVMetalTexture&& other) noexcept;
  ScopedCVMetalTexture& operator=(ScopedCVMetalTexture&& other) noexcept;

  void reset(CVMetalTextureRef texture = nullptr);
  CVMetalTextureRef get() const { return texture_; }
  id<MTLTexture> texture() const;
  bool valid() const;

 private:
  CVMetalTextureRef texture_ = nullptr;
};

CVReturn create_cv_metal_texture(CVMetalTextureCacheRef cache,
                                 CVPixelBufferRef pixelBuffer,
                                 MTLPixelFormat pixelFormat,
                                 size_t width,
                                 size_t height,
                                 size_t planeIndex,
                                 ScopedCVMetalTexture* out);

}  // namespace vp_macos

#endif  // VOIDPLAYER_MACOS_METAL_TEXTURE_WRAPPING_H_
