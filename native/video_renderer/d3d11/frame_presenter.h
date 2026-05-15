#pragma once

#include "video_renderer/buffer/bidi_ring_buffer.h"
#include "video_renderer/d3d11/texture.h"
#include <array>
#include <functional>
#include <mutex>
#include <wrl/client.h>

namespace vr {

struct D3D11PreparedFrame {
    ID3D11ShaderResourceView* rgba_srv = nullptr;
    ID3D11ShaderResourceView* nv12_y_srv = nullptr;
    ID3D11ShaderResourceView* nv12_uv_srv = nullptr;
    ID3D11ShaderResourceView* planar_u_srv = nullptr;
    ID3D11ShaderResourceView* planar_v_srv = nullptr;
    float nv12_uv_scale_x = 1.0f;
    float nv12_uv_scale_y = 1.0f;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> owned_rgba_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> owned_nv12_y_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> owned_nv12_uv_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> owned_planar_u_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> owned_planar_v_srv;
};

struct D3D11FramePresenterSlotMemoryStats {
    uint64_t software_texture_bytes = 0;
    uint64_t software_nv12_texture_bytes = 0;
    uint64_t software_planar_texture_bytes = 0;
    uint64_t render_nv12_copy_texture_bytes = 0;
    int render_nv12_copy_width = 0;
    int render_nv12_copy_height = 0;
    int render_nv12_copy_format = 0;
};

struct D3D11FramePresenterMemoryStats {
    std::array<D3D11FramePresenterSlotMemoryStats, 4> slots{};
    uint64_t total_estimated_bytes = 0;
};

class D3D11FramePresenter {
public:
    static constexpr size_t kMaxSlots = 4;
    using GpuIdleWait = std::function<void(const char*)>;

    D3D11FramePresenter(TextureManager* texture_manager, ID3D11DeviceContext* context);

    bool prepare_frame(size_t slot,
                       const TextureFrame& frame,
                       int fallback_width,
                       int fallback_height,
                       const GpuIdleWait& wait_gpu_idle,
                       D3D11PreparedFrame& out);

    float nv12_uv_scale_x(size_t slot) const;
    float nv12_uv_scale_y(size_t slot) const;
    void reset_track(size_t slot);
    void move_track(size_t from, size_t to);
    void reset_all();
    D3D11FramePresenterMemoryStats memory_stats() const;

private:
    struct TrackResources {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sw_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sw_srv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sw_nv12_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sw_nv12_y_srv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sw_nv12_uv_srv;
        std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 3> sw_planar_textures;
        std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 3> sw_planar_srvs;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nv12_y_srv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nv12_uv_srv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> render_nv12_tex;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> render_nv12_copy_tex;
        void* last_nv12_tex = nullptr;
        int last_nv12_idx = -1;
        float nv12_uv_scale_x = 1.0f;
        float nv12_uv_scale_y = 1.0f;
    };

    bool prepare_nv12_frame(size_t slot,
                            const TextureFrame& frame,
                            const GpuIdleWait& wait_gpu_idle,
                            D3D11PreparedFrame& out);
    bool prepare_software_frame(size_t slot,
                                const TextureFrame& frame,
                                int fallback_width,
                                int fallback_height,
                                D3D11PreparedFrame& out);
    bool prepare_software_nv12_frame(size_t slot,
                                     const TextureFrame& frame,
                                     int fallback_width,
                                     int fallback_height,
                                     D3D11PreparedFrame& out);
    bool prepare_software_planar_yuv_frame(size_t slot,
                                           const TextureFrame& frame,
                                           D3D11PreparedFrame& out);
    bool prepare_texture_frame(const TextureFrame& frame, D3D11PreparedFrame& out);

    TextureManager* texture_manager_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    mutable std::mutex mutex_;
    std::array<TrackResources, kMaxSlots> tracks_{};
};

} // namespace vr
