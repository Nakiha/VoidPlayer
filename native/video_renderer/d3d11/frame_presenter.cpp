#include "video_renderer/d3d11/frame_presenter.h"
#include <spdlog/spdlog.h>
#include <utility>

namespace vr {

D3D11FramePresenter::D3D11FramePresenter(TextureManager* texture_manager,
                                         ID3D11DeviceContext* context)
    : texture_manager_(texture_manager), context_(context) {}

bool D3D11FramePresenter::prepare_frame(size_t slot,
                                        const TextureFrame& frame,
                                        int fallback_width,
                                        int fallback_height,
                                        const GpuIdleWait& wait_gpu_idle,
                                        D3D11PreparedFrame& out) {
    out = {};
    if (slot >= tracks_.size() || !frame.texture_handle) {
        return false;
    }

    if (frame.is_ref && frame.is_nv12) {
        return prepare_nv12_frame(slot, frame, wait_gpu_idle, out);
    }
    if (frame.is_ref) {
        return prepare_texture_frame(frame, out);
    }
    return prepare_software_frame(slot, frame, fallback_width, fallback_height, out);
}

float D3D11FramePresenter::nv12_uv_scale_y(size_t slot) const {
    if (slot >= tracks_.size()) {
        return 1.0f;
    }
    return tracks_[slot].nv12_uv_scale_y;
}

void D3D11FramePresenter::reset_track(size_t slot) {
    if (slot >= tracks_.size()) {
        return;
    }
    tracks_[slot] = TrackResources{};
}

void D3D11FramePresenter::move_track(size_t from, size_t to) {
    if (from >= tracks_.size() || to >= tracks_.size() || from == to) {
        return;
    }
    tracks_[to] = std::move(tracks_[from]);
    tracks_[from] = TrackResources{};
}

void D3D11FramePresenter::reset_all() {
    for (size_t i = 0; i < tracks_.size(); ++i) {
        reset_track(i);
    }
}

bool D3D11FramePresenter::prepare_nv12_frame(size_t slot,
                                             const TextureFrame& frame,
                                             const GpuIdleWait& wait_gpu_idle,
                                             D3D11PreparedFrame& out) {
    auto& resources = tracks_[slot];
    auto* decode_tex = static_cast<ID3D11Texture2D*>(frame.texture_handle);
    const int array_idx = frame.texture_array_index;
    if (!decode_tex || array_idx < 0 || !texture_manager_ || !context_) {
        spdlog::error("[D3D11FramePresenter] Invalid NV12 frame for slot {}", slot);
        return false;
    }

    const bool opened_new_shared_resource = resources.last_nv12_tex != decode_tex;
    if (opened_new_shared_resource) {
        resources.render_nv12_tex.Reset();
        resources.last_nv12_tex = nullptr;

        if (!texture_manager_->open_shared_texture(decode_tex, resources.render_nv12_tex)) {
            spdlog::error("[D3D11FramePresenter] Failed to open shared NV12 texture for slot {}",
                          slot);
            return false;
        }
        resources.last_nv12_tex = decode_tex;
    }

    if (!resources.render_nv12_tex) {
        return false;
    }

    D3D11_TEXTURE2D_DESC src_desc = {};
    resources.render_nv12_tex->GetDesc(&src_desc);
    if (static_cast<UINT>(array_idx) >= src_desc.ArraySize) {
        spdlog::error("[D3D11FramePresenter] NV12 array index out of range for slot {}: idx={}, array_size={}",
                      slot, array_idx, src_desc.ArraySize);
        return false;
    }

    bool created_new_copy_texture = false;
    if (!texture_manager_->ensure_nv12_copy_resources(
            resources.render_nv12_tex.Get(),
            resources.render_nv12_copy_tex,
            resources.nv12_y_srv,
            resources.nv12_uv_srv,
            &created_new_copy_texture)) {
        spdlog::error("[D3D11FramePresenter] Failed to prepare NV12 resources for slot {}",
                      slot);
        return false;
    }

    auto copy_nv12_slice = [&] {
        context_->CopySubresourceRegion(
            resources.render_nv12_copy_tex.Get(),
            0,
            0, 0, 0,
            resources.render_nv12_tex.Get(),
            D3D11CalcSubresource(0, static_cast<UINT>(array_idx), 1),
            nullptr);
    };
    copy_nv12_slice();

    if (opened_new_shared_resource || created_new_copy_texture) {
        wait_gpu_idle("D3D11FramePresenter::prepare_nv12_frame");
        copy_nv12_slice();
        wait_gpu_idle("D3D11FramePresenter::prepare_nv12_frame");
    }

    if (src_desc.Height > 0 && frame.height > 0 &&
        src_desc.Height != static_cast<UINT>(frame.height)) {
        resources.nv12_uv_scale_y =
            static_cast<float>(frame.height) / static_cast<float>(src_desc.Height);
    } else {
        resources.nv12_uv_scale_y = 1.0f;
    }

    resources.last_nv12_idx = array_idx;
    out.nv12_y_srv = resources.nv12_y_srv.Get();
    out.nv12_uv_srv = resources.nv12_uv_srv.Get();
    return out.nv12_y_srv && out.nv12_uv_srv;
}

bool D3D11FramePresenter::prepare_software_frame(size_t slot,
                                                 const TextureFrame& frame,
                                                 int fallback_width,
                                                 int fallback_height,
                                                 D3D11PreparedFrame& out) {
    auto& resources = tracks_[slot];
    if (!texture_manager_) {
        return false;
    }

    const int w = frame.width > 0 ? frame.width : fallback_width;
    const int h = frame.height > 0 ? frame.height : fallback_height;

    bool need_new_tex = !resources.sw_texture;
    if (resources.sw_texture) {
        D3D11_TEXTURE2D_DESC existing_desc = {};
        resources.sw_texture->GetDesc(&existing_desc);
        need_new_tex =
            static_cast<int>(existing_desc.Width) != w ||
            static_cast<int>(existing_desc.Height) != h;
    }

    if (need_new_tex) {
        resources.sw_srv.Reset();
        resources.sw_texture.Attach(texture_manager_->create_rgba_texture(w, h));
        if (resources.sw_texture) {
            resources.sw_srv.Attach(texture_manager_->create_srv(resources.sw_texture.Get()));
        }
    }

    if (!resources.sw_texture || !resources.sw_srv) {
        return false;
    }

    int stride = w * 4;
    if (const auto* storage = frame.cpu_rgba_storage()) {
        if (storage->stride > 0) {
            stride = storage->stride;
        }
    }
    if (!texture_manager_->upload_data(
            resources.sw_texture.Get(),
            static_cast<const uint8_t*>(frame.texture_handle),
            w, h, stride)) {
        return false;
    }

    out.rgba_srv = resources.sw_srv.Get();
    return true;
}

bool D3D11FramePresenter::prepare_texture_frame(const TextureFrame& frame,
                                                D3D11PreparedFrame& out) {
    if (!texture_manager_) {
        return false;
    }
    auto* tex = static_cast<ID3D11Texture2D*>(frame.texture_handle);
    out.owned_rgba_srv.Attach(texture_manager_->create_srv(tex));
    out.rgba_srv = out.owned_rgba_srv.Get();
    return out.rgba_srv != nullptr;
}

} // namespace vr
