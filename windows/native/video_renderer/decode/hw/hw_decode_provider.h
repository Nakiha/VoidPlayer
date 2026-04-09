#pragma once
#include <cstdint>
#include <mutex>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace vr {

enum class HwDecodeType {
    None = 0,
    D3D11VA,
    CUDA,
    DXVA2,
    Vulkan,
};

/// Abstract interface for hardware decode providers.
/// Each backend (D3D11VA, CUDA, etc.) implements this interface.
/// The factory function try_hw_decode_providers() probes providers in priority order.
class HwDecodeProvider {
public:
    virtual ~HwDecodeProvider() = default;

    /// Check if this provider can accelerate the given codec.
    virtual bool probe(const AVCodec* codec) const = 0;

    /// Initialize the hardware device context for decoding.
    /// @param native_device  Platform-specific device (e.g. ID3D11Device*)
    /// @param width          Video frame width
    /// @param height         Video frame height
    /// @param device_mutex   Optional mutex for D3D11 immediate context serialization.
    ///                       Must outlive the hw_device_ctx. When provided, the provider
    ///                       uses it instead of creating (and destroying) its own.
    /// @return Result with hw_device_ctx on success, or success=false on failure.
    struct HwDecodeInitResult;

    virtual HwDecodeInitResult init(void* native_device, int width, int height,
                                    std::recursive_mutex* device_mutex = nullptr) = 0;

    /// Release provider-held resources (not hw_device_ctx, which caller owns).
    virtual void shutdown() = 0;

    /// Flush the hardware decode device context to ensure GPU commands are
    /// submitted. Required for cross-device shared resource visibility —
    /// DXGI mandates Flush() on the producing device before the consuming
    /// device can read shared texture data.
    virtual void flush() = 0;

    virtual HwDecodeType type() const = 0;
    virtual const char* name() const = 0;
};

/// Result of hardware decode initialization. Defined outside the class to allow
/// the unique_ptr<HwDecodeProvider> member (class is complete at this point).
struct HwDecodeProvider::HwDecodeInitResult {
    bool success = false;
    AVBufferRef* hw_device_ctx = nullptr;   // Ownership transferred to caller
    AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
    HwDecodeType type = HwDecodeType::None;
    std::unique_ptr<HwDecodeProvider> provider;  // Must outlive hw_device_ctx (owns mutex, device context)
};

/// Convenience alias to avoid repeating the qualified name.
using HwDecodeInitResult = HwDecodeProvider::HwDecodeInitResult;

/// Factory: try each registered provider in priority order, return first success.
HwDecodeInitResult try_hw_decode_providers(
    void* native_device,
    const AVCodec* codec,
    int width,
    int height,
    std::recursive_mutex* device_mutex = nullptr);

} // namespace vr
