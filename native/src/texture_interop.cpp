#include "voidview_native/texture_interop.hpp"

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <GL/gl.h>
#endif

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <cstdio>
#include <cstring>

// OpenGL constants
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

namespace voidview {

#ifdef _WIN32
// WGL_NV_DX_interop function pointers
typedef BOOL (WINAPI *PFNWGLDXSETRESOURCESHAREHANDLEPROC)(void *dxObject, HANDLE shareHandle);
typedef HANDLE (WINAPI *PFNWGLDXOPENDEVICENVPROC)(void *dxDevice);
typedef BOOL (WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE (WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL (WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL (WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL (WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);

#define WGL_ACCESS_READ_ONLY_NV 0x0000

static PFNWGLDXSETRESOURCESHAREHANDLEPROC wglDXSetResourceShareHandleNV = nullptr;
static PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV = nullptr;
static PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV = nullptr;
static PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV = nullptr;
static PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV = nullptr;
static PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV = nullptr;
static PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV = nullptr;

typedef const char* (WINAPI *PFNWGLGETEXTENSIONSSTRINGARBPROC)(HDC hdc);
static PFNWGLGETEXTENSIONSSTRINGARBPROC wglGetExtensionsStringARB = nullptr;

static bool wgl_extensions_loaded = false;

static bool load_wgl_extensions() {
    if (wgl_extensions_loaded) return wglDXOpenDeviceNV != nullptr;
    wgl_extensions_loaded = true;

    HDC dc = wglGetCurrentDC();
    if (!dc) {
        fprintf(stderr, "No WGL context current\n");
        return false;
    }

    wglGetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC)
        wglGetProcAddress("wglGetExtensionsStringARB");
    if (!wglGetExtensionsStringARB) {
        fprintf(stderr, "wglGetExtensionsStringARB not available\n");
        return false;
    }

    const char* extensions = wglGetExtensionsStringARB(dc);
    if (!extensions || !strstr(extensions, "WGL_NV_DX_interop")) {
        fprintf(stderr, "WGL_NV_DX_interop not available\n");
        return false;
    }

    wglDXSetResourceShareHandleNV = (PFNWGLDXSETRESOURCESHAREHANDLEPROC)
        wglGetProcAddress("wglDXSetResourceShareHandleNV");
    wglDXOpenDeviceNV = (PFNWGLDXOPENDEVICENVPROC)
        wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV = (PFNWGLDXCLOSEDEVICENVPROC)
        wglGetProcAddress("wglDXCloseDeviceNV");
    wglDXRegisterObjectNV = (PFNWGLDXREGISTEROBJECTNVPROC)
        wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV = (PFNWGLDXUNREGISTEROBJECTNVPROC)
        wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (PFNWGLDXLOCKOBJECTSNVPROC)
        wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (PFNWGLDXUNLOCKOBJECTSNVPROC)
        wglGetProcAddress("wglDXUnlockObjectsNV");

    if (!wglDXOpenDeviceNV || !wglDXRegisterObjectNV || !wglDXLockObjectsNV) {
        fprintf(stderr, "Failed to load WGL_NV_DX_interop functions\n");
        return false;
    }

    printf("WGL_NV_DX_interop loaded successfully\n");
    return true;
}
#endif

TextureInterop::TextureInterop() = default;

TextureInterop::~TextureInterop() {
    release();
}

bool TextureInterop::initialize(ID3D11Device* shared_device) {
    if (initialized_) return true;

    if (!shared_device) {
        fprintf(stderr, "TextureInterop: No D3D11 device provided\n");
        return false;
    }

#ifdef _WIN32
    d3d11_device_ = shared_device;
    d3d11_device_->AddRef();
    owns_device_ = false;

    // Get device context
    d3d11_device_->GetImmediateContext(&d3d11_context_);
    if (!d3d11_context_) {
        fprintf(stderr, "Failed to get D3D11 context\n");
        return false;
    }

    // Query video device and context for VideoProcessor
    HRESULT hr = d3d11_device_->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&video_device_);
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to query ID3D11VideoDevice: 0x%08X (zero-copy disabled)\n", hr);
        video_device_ = nullptr;
    }

    hr = d3d11_context_->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&video_context_);
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to query ID3D11VideoContext: 0x%08X (zero-copy disabled)\n", hr);
        video_context_ = nullptr;
    }

    if (video_device_ && video_context_) {
        printf("D3D11 VideoProcessor available (zero-copy enabled)\n");
    }

    printf("TextureInterop using FFmpeg's D3D11 device: %p\n", d3d11_device_);

    // Load WGL extensions
    if (!load_wgl_extensions()) {
        fprintf(stderr, "WGL_NV_DX_interop not available\n");
        return false;
    }

    // Open WGL interop device
    wgl_device_ = wglDXOpenDeviceNV(d3d11_device_);
    if (!wgl_device_) {
        fprintf(stderr, "wglDXOpenDeviceNV failed\n");
        return false;
    }
    printf("WGL interop device opened\n");

    initialized_ = true;
    return true;
#else
    (void)shared_device;
    return false;
#endif
}

ID3D11Device* TextureInterop::get_d3d11_device() {
    return d3d11_device_;
}

bool TextureInterop::init_gl_resources(int width, int height) {
    if (width_ == width && height_ == height && gl_texture_rgba_ != 0) {
        return true;
    }

    release_gl_resources();

    width_ = width;
    height_ = height;

#ifdef _WIN32
    // Create shared RGBA texture in D3D11
    D3D11_TEXTURE2D_DESC rgba_desc = {};
    rgba_desc.Width = width;
    rgba_desc.Height = height;
    rgba_desc.MipLevels = 1;
    rgba_desc.ArraySize = 1;
    rgba_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rgba_desc.SampleDesc.Count = 1;
    rgba_desc.Usage = D3D11_USAGE_DEFAULT;
    rgba_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    rgba_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = d3d11_device_->CreateTexture2D(&rgba_desc, nullptr, &rgba_texture_);
    if (FAILED(hr)) {
        fprintf(stderr, "Failed to create shared RGBA texture: 0x%08X\n", hr);
        return false;
    }

    printf("Created shared D3D11 RGBA texture %p (%dx%d)\n", rgba_texture_, width, height);
#endif

    // Create OpenGL texture
    glGenTextures(1, &gl_texture_rgba_);
    if (!gl_texture_rgba_) {
        fprintf(stderr, "Failed to create GL texture\n");
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, gl_texture_rgba_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    printf("Created GL texture %u (%dx%d)\n", gl_texture_rgba_, width, height);

    // Register with WGL interop
    if (!wgl_object_) {
        HANDLE share_handle = nullptr;
        IDXGIResource* dxgi_resource = nullptr;
        HRESULT hr = rgba_texture_->QueryInterface(__uuidof(IDXGIResource), (void**)&dxgi_resource);
        if (SUCCEEDED(hr) && dxgi_resource) {
            dxgi_resource->GetSharedHandle(&share_handle);
            dxgi_resource->Release();
        }

        if (share_handle && wglDXSetResourceShareHandleNV) {
            wglDXSetResourceShareHandleNV(rgba_texture_, share_handle);
        }

        wgl_object_ = wglDXRegisterObjectNV(
            wgl_device_,
            rgba_texture_,
            gl_texture_rgba_,
            GL_TEXTURE_2D,
            WGL_ACCESS_READ_ONLY_NV
        );

        if (!wgl_object_) {
            fprintf(stderr, "wglDXRegisterObjectNV failed\n");
            return false;
        }
        printf("Registered shared texture with OpenGL\n");
    }

    return true;
}

#ifdef _WIN32
bool TextureInterop::init_video_processor(int width, int height, DXGI_FORMAT input_format) {
    if (!video_device_ || !video_context_) {
        return false;
    }

    release_video_processor();

    // Create video processor enumerator
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc = {};
    content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content_desc.InputFrameRate.Numerator = 30;
    content_desc.InputFrameRate.Denominator = 1;
    content_desc.InputWidth = width;
    content_desc.InputHeight = height;
    content_desc.OutputFrameRate.Numerator = 30;
    content_desc.OutputFrameRate.Denominator = 1;
    content_desc.OutputWidth = width;
    content_desc.OutputHeight = height;
    content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&content_desc, &video_processor_enum_);
    if (FAILED(hr)) {
        fprintf(stderr, "CreateVideoProcessorEnumerator failed: 0x%08X\n", hr);
        return false;
    }

    // Check if format is supported
    UINT flags;
    hr = video_processor_enum_->CheckVideoProcessorFormat(input_format, &flags);
    if (FAILED(hr) || !(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        fprintf(stderr, "VideoProcessor doesn't support input format: 0x%08X\n", input_format);
        return false;
    }

    // Create video processor
    hr = video_device_->CreateVideoProcessor(video_processor_enum_, 0, &video_processor_);
    if (FAILED(hr)) {
        fprintf(stderr, "CreateVideoProcessor failed: 0x%08X\n", hr);
        return false;
    }

    printf("D3D11 VideoProcessor created for %dx%d (format=%d)\n", width, height, input_format);
    return true;
}

bool TextureInterop::convert_nv12_to_rgba_gpu(ID3D11Texture2D* nv12_texture, int subresource) {
    if (!video_processor_ || !video_context_ || !rgba_texture_) {
        return false;
    }

    // Create input view for NV12 texture
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc = {};
    input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_view_desc.Texture2D.ArraySlice = subresource;
    input_view_desc.Texture2D.MipSlice = 0;

    ID3D11VideoProcessorInputView* input_view = nullptr;
    HRESULT hr = video_device_->CreateVideoProcessorInputView(
        nv12_texture, video_processor_enum_, &input_view_desc, &input_view);
    if (FAILED(hr)) {
        fprintf(stderr, "CreateVideoProcessorInputView failed: 0x%08X\n", hr);
        return false;
    }

    // Create output view for RGBA texture
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc = {};
    output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_view_desc.Texture2D.MipSlice = 0;

    ID3D11VideoProcessorOutputView* output_view = nullptr;
    hr = video_device_->CreateVideoProcessorOutputView(
        rgba_texture_, video_processor_enum_, &output_view_desc, &output_view);
    if (FAILED(hr)) {
        fprintf(stderr, "CreateVideoProcessorOutputView failed: 0x%08X\n", hr);
        input_view->Release();
        return false;
    }

    // Perform the conversion (GPU only)
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view;

    hr = video_context_->VideoProcessorBlt(video_processor_, output_view, 0, 1, &stream);
    if (FAILED(hr)) {
        fprintf(stderr, "VideoProcessorBlt failed: 0x%08X\n", hr);
        input_view->Release();
        output_view->Release();
        return false;
    }

    input_view->Release();
    output_view->Release();

    printf("GPU NV12->RGBA conversion successful\n");
    return true;
}
#endif

bool TextureInterop::bind_frame(AVFrame* frame) {
#ifdef _WIN32
    if (!initialized_ || !frame) {
        fprintf(stderr, "bind_frame: invalid state\n");
        return false;
    }

    // Unlock previous frame if locked
    if (wgl_object_ && bound_) {
        HANDLE objects[] = { wgl_object_ };
        wglDXUnlockObjectsNV(wgl_device_, 1, objects);
        bound_ = false;
    }

    int width = frame->width;
    int height = frame->height;

    // Get D3D11 texture from AVFrame
    ID3D11Texture2D* nv12_tex = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    int subresource = static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));

    if (!nv12_tex) {
        fprintf(stderr, "No D3D11 texture in frame\n");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    nv12_tex->GetDesc(&desc);

    printf("Frame: %dx%d, format=%u, subresource=%d\n", width, height, desc.Format, subresource);

    // Initialize GL resources
    if (!init_gl_resources(width, height)) {
        return false;
    }

    // Try GPU path first (zero-copy)
    bool gpu_success = false;
    if (video_device_ && video_context_) {
        // Initialize video processor if needed
        if (!video_processor_) {
            if (!init_video_processor(width, height, desc.Format)) {
                fprintf(stderr, "VideoProcessor init failed, falling back to CPU\n");
            }
        }

        if (video_processor_) {
            gpu_success = convert_nv12_to_rgba_gpu(nv12_tex, subresource);
        }
    }

    // Fallback to CPU path if GPU failed
    if (!gpu_success) {
        printf("Using CPU fallback for YUV conversion\n");

        AVFrame* sw_frame = av_frame_alloc();
        if (!sw_frame) {
            fprintf(stderr, "Failed to allocate software frame\n");
            return false;
        }

        int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
        if (ret < 0) {
            fprintf(stderr, "av_hwframe_transfer_data failed: %d\n", ret);
            av_frame_free(&sw_frame);
            return false;
        }

        const uint8_t* y_data = sw_frame->data[0];
        const uint8_t* uv_data = sw_frame->data[1];
        int y_linesize = sw_frame->linesize[0];
        int uv_linesize = sw_frame->linesize[1];

        // Create staging texture
        D3D11_TEXTURE2D_DESC staging_desc = {};
        staging_desc.Width = width;
        staging_desc.Height = height;
        staging_desc.MipLevels = 1;
        staging_desc.ArraySize = 1;
        staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        staging_desc.SampleDesc.Count = 1;
        staging_desc.Usage = D3D11_USAGE_DYNAMIC;
        staging_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        ID3D11Texture2D* staging = nullptr;
        HRESULT hr = d3d11_device_->CreateTexture2D(&staging_desc, nullptr, &staging);
        if (FAILED(hr)) {
            fprintf(stderr, "Failed to create staging texture: 0x%08X\n", hr);
            av_frame_free(&sw_frame);
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = d3d11_context_->Map(staging, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            uint8_t* dst = static_cast<uint8_t*>(mapped.pData);

            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int y_idx = y * y_linesize + x;
                    int uv_x = x / 2;
                    int uv_y = y / 2;
                    int uv_idx = uv_y * uv_linesize + uv_x * 2;

                    uint8_t y_val = y_data[y_idx];
                    uint8_t u_val = uv_data[uv_idx];
                    uint8_t v_val = uv_data[uv_idx + 1];

                    int c = y_val - 16;
                    int d = u_val - 128;
                    int e = v_val - 128;

                    int r = (298 * c + 409 * e + 128) >> 8;
                    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                    int b = (298 * c + 516 * d + 128) >> 8;

                    r = r < 0 ? 0 : (r > 255 ? 255 : r);
                    g = g < 0 ? 0 : (g > 255 ? 255 : g);
                    b = b < 0 ? 0 : (b > 255 ? 255 : b);

                    int dst_idx = y * mapped.RowPitch + x * 4;
                    dst[dst_idx + 0] = b;
                    dst[dst_idx + 1] = g;
                    dst[dst_idx + 2] = r;
                    dst[dst_idx + 3] = 255;
                }
            }

            d3d11_context_->Unmap(staging, 0);
            d3d11_context_->CopySubresourceRegion(rgba_texture_, 0, 0, 0, 0, staging, 0, nullptr);
        }

        staging->Release();
        av_frame_free(&sw_frame);
    }

    // Lock for OpenGL access - keep locked until next frame
    if (wgl_object_) {
        HANDLE objects[] = { wgl_object_ };
        wglDXLockObjectsNV(wgl_device_, 1, objects);
    }

    bound_ = true;
    printf("Frame bound to GL texture %u (locked for GL)\n", gl_texture_rgba_);
    return true;
#else
    (void)frame;
    return false;
#endif
}

void TextureInterop::release_video_processor() {
#ifdef _WIN32
    if (video_processor_) {
        video_processor_->Release();
        video_processor_ = nullptr;
    }
    if (video_processor_enum_) {
        video_processor_enum_->Release();
        video_processor_enum_ = nullptr;
    }
#endif
}

void TextureInterop::release_interop() {
#ifdef _WIN32
    if (wgl_object_ && bound_) {
        HANDLE objects[] = { wgl_object_ };
        wglDXUnlockObjectsNV(wgl_device_, 1, objects);
    }
    release_video_processor();

    if (wgl_object_) {
        wglDXUnregisterObjectNV(wgl_device_, wgl_object_);
        wgl_object_ = nullptr;
    }
    if (wgl_device_) {
        wglDXCloseDeviceNV(wgl_device_);
        wgl_device_ = nullptr;
    }
    if (rgba_texture_) {
        rgba_texture_->Release();
        rgba_texture_ = nullptr;
    }
    if (video_context_) {
        video_context_->Release();
        video_context_ = nullptr;
    }
    if (video_device_) {
        video_device_->Release();
        video_device_ = nullptr;
    }
    if (d3d11_context_) {
        d3d11_context_->Release();
        d3d11_context_ = nullptr;
    }
    if (d3d11_device_) {
        if (owns_device_) {
            d3d11_device_->Release();
        }
        d3d11_device_ = nullptr;
    }
    bound_ = false;
#endif
}

void TextureInterop::release_gl_resources() {
    if (gl_texture_rgba_) {
        glDeleteTextures(1, &gl_texture_rgba_);
        gl_texture_rgba_ = 0;
    }
}

void TextureInterop::release() {
    release_interop();
    release_gl_resources();
    initialized_ = false;
}

uint32_t TextureInterop::get_texture_y_id() const { return 0; }
uint32_t TextureInterop::get_texture_uv_id() const { return 0; }
uint32_t TextureInterop::get_texture_id() const { return gl_texture_rgba_; }
bool TextureInterop::convert_to_rgba() { return bound_; }
bool TextureInterop::is_initialized() const { return initialized_; }

} // namespace voidview
