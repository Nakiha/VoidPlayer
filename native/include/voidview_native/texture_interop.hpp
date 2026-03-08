#pragma once

#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
}

#ifdef _WIN32
#include <d3d11.h>
#include <d3d11_1.h>  // For ID3D11VideoProcessor
#endif

namespace voidview {

/**
 * Texture Interop
 *
 * Handles D3D11 hardware frame to OpenGL texture sharing.
 * Uses WGL_NV_DX_interop for zero-copy texture sharing.
 * Uses D3D11 VideoProcessor for GPU-based NV12→RGBA conversion.
 */
class TextureInterop {
public:
    TextureInterop();
    ~TextureInterop();

    // Disable copy
    TextureInterop(const TextureInterop&) = delete;
    TextureInterop& operator=(const TextureInterop&) = delete;

    /**
     * Initialize interop with shared D3D11 device
     * @param shared_device Optional existing D3D11 device to share
     * @return True on success
     */
    bool initialize(ID3D11Device* shared_device = nullptr);

    /**
     * Get the D3D11 device for FFmpeg hardware context
     * @return D3D11 device pointer (owned by this class)
     */
    ID3D11Device* get_d3d11_device();

    /**
     * Bind hardware frame to texture (zero-copy)
     * @param frame AVFrame containing D3D11 texture
     * @return True on success
     */
    bool bind_frame(AVFrame* frame);

    /**
     * Release current binding
     */
    void release();

    /**
     * Get OpenGL Y-plane texture ID
     */
    uint32_t get_texture_y_id() const;

    /**
     * Get OpenGL UV-plane texture ID
     */
    uint32_t get_texture_uv_id() const;

    /**
     * Get OpenGL RGBA texture ID
     */
    uint32_t get_texture_id() const;

    /**
     * Convert bound NV12 to RGBA using shader (no-op if already converted)
     */
    bool convert_to_rgba();

    /**
     * Check if initialized
     */
    bool is_initialized() const;

    /**
     * Get frame width
     */
    int get_width() const { return width_; }

    /**
     * Get frame height
     */
    int get_height() const { return height_; }

private:
    bool init_gl_resources(int width, int height);
    bool init_video_processor(int width, int height, DXGI_FORMAT input_format);
    bool convert_nv12_to_rgba_gpu(ID3D11Texture2D* nv12_texture, int subresource);
    void release_interop();
    void release_gl_resources();
    void release_video_processor();

    // Dimensions
    int width_ = 0;
    int height_ = 0;

    bool initialized_ = false;
    bool bound_ = false;

#ifdef _WIN32
    // D3D11 resources
    ID3D11Device* d3d11_device_ = nullptr;
    ID3D11DeviceContext* d3d11_context_ = nullptr;
    bool owns_device_ = false;

    // VideoProcessor for GPU NV12→RGBA conversion
    ID3D11VideoDevice* video_device_ = nullptr;
    ID3D11VideoContext* video_context_ = nullptr;
    ID3D11VideoProcessorEnumerator* video_processor_enum_ = nullptr;
    ID3D11VideoProcessor* video_processor_ = nullptr;

    // Shared RGBA texture (for WGL interop)
    ID3D11Texture2D* rgba_texture_ = nullptr;

    // WGL interop resources
    HANDLE wgl_device_ = nullptr;
    HANDLE wgl_object_ = nullptr;
#endif

    // OpenGL resources
    uint32_t gl_texture_rgba_ = 0;
};

} // namespace voidview
