#pragma once

#include "video_renderer/buffer/bidi_ring_buffer.h"
#include "video_renderer/d3d11/texture.h"
#include <array>
#include <functional>
#include <wrl/client.h>

namespace vr {

struct D3D11PreparedFrame {
    ID3D11ShaderResourceView* rgba_srv = nullptr;
    ID3D11ShaderResourceView* nv12_y_srv = nullptr;
    ID3D11ShaderResourceView* nv12_uv_srv = nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> owned_rgba_srv;
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

    float nv12_uv_scale_y(size_t slot) const;
    void reset_track(size_t slot);
    void move_track(size_t from, size_t to);
    void reset_all();

private:
    struct TrackResources {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sw_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sw_srv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nv12_y_srv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nv12_uv_srv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> render_nv12_tex;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> render_nv12_copy_tex;
        void* last_nv12_tex = nullptr;
        int last_nv12_idx = -1;
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
    bool prepare_texture_frame(const TextureFrame& frame, D3D11PreparedFrame& out);

    TextureManager* texture_manager_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    std::array<TrackResources, kMaxSlots> tracks_{};
};

} // namespace vr
