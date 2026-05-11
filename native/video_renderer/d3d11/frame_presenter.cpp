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

    if (frame.cpu_planar_yuv_storage()) {
        return prepare_software_planar_yuv_frame(slot, frame, out);
    }
    if (frame.is_nv12) {
        if (!frame.is_ref) {
            return prepare_software_nv12_frame(slot, frame, fallback_width, fallback_height, out);
        }
        return prepare_nv12_frame(slot, frame, wait_gpu_idle, out);
    }
    if (frame.is_ref) {
        return prepare_texture_frame(frame, out);
    }
    return prepare_software_frame(slot, frame, fallback_width, fallback_height, out);
}

float D3D11FramePresenter::nv12_uv_scale_x(size_t slot) const {
    if (slot >= tracks_.size()) {
        return 1.0f;
    }
    return tracks_[slot].nv12_uv_scale_x;
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
        resources.nv12_y_srv.Reset();
        resources.nv12_uv_srv.Reset();
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

    const bool can_sample_directly =
        src_desc.ArraySize == 1 &&
        array_idx == 0 &&
        (src_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    if (can_sample_directly) {
        resources.render_nv12_copy_tex.Reset();
        if (!resources.nv12_y_srv || !resources.nv12_uv_srv) {
            if (!texture_manager_->create_nv12_plane_srvs(
                    resources.render_nv12_tex.Get(),
                    resources.nv12_y_srv,
                    resources.nv12_uv_srv)) {
                spdlog::error("[D3D11FramePresenter] Failed to prepare direct NV12 SRVs for slot {}",
                              slot);
                return false;
            }
        }
    } else {
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
    }

    if (src_desc.Width > 0 && frame.width > 0 &&
        static_cast<UINT>(frame.width) < src_desc.Width) {
        resources.nv12_uv_scale_x =
            static_cast<float>(frame.width) / static_cast<float>(src_desc.Width);
    } else {
        resources.nv12_uv_scale_x = 1.0f;
    }

    if (src_desc.Height > 0 && frame.height > 0 &&
        static_cast<UINT>(frame.height) < src_desc.Height) {
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

bool D3D11FramePresenter::prepare_software_nv12_frame(size_t slot,
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
    if ((w & 1) != 0 || (h & 1) != 0) {
        spdlog::error("[D3D11FramePresenter] Invalid CPU NV12 frame geometry ({}x{})",
                      w, h);
        return false;
    }

    bool need_new_tex = !resources.sw_nv12_texture;
    if (resources.sw_nv12_texture) {
        D3D11_TEXTURE2D_DESC existing_desc = {};
        resources.sw_nv12_texture->GetDesc(&existing_desc);
        const DXGI_FORMAT expected_format = frame.is_p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        need_new_tex =
            static_cast<int>(existing_desc.Width) != w ||
            static_cast<int>(existing_desc.Height) != h ||
            existing_desc.Format != expected_format;
    }

    if (need_new_tex) {
        resources.sw_nv12_y_srv.Reset();
        resources.sw_nv12_uv_srv.Reset();
        resources.sw_nv12_texture.Attach(frame.is_p010
            ? texture_manager_->create_p010_texture(w, h)
            : texture_manager_->create_nv12_texture(w, h));
        if (resources.sw_nv12_texture) {
            texture_manager_->create_nv12_plane_srvs(
                resources.sw_nv12_texture.Get(),
                resources.sw_nv12_y_srv,
                resources.sw_nv12_uv_srv);
        }
    }

    if (!resources.sw_nv12_texture ||
        !resources.sw_nv12_y_srv ||
        !resources.sw_nv12_uv_srv) {
        return false;
    }

    int y_stride = w;
    int uv_stride = w;
    if (const auto* storage = frame.cpu_nv12_storage()) {
        if (storage->y_stride > 0) {
            y_stride = storage->y_stride;
        }
        if (storage->uv_stride > 0) {
            uv_stride = storage->uv_stride;
        }
    }

    if (!texture_manager_->upload_nv12_data(
            resources.sw_nv12_texture.Get(),
            static_cast<const uint8_t*>(frame.texture_handle),
            w, h, y_stride, uv_stride, frame.is_p010)) {
        return false;
    }

    resources.nv12_uv_scale_x = 1.0f;
    resources.nv12_uv_scale_y = 1.0f;
    out.nv12_y_srv = resources.sw_nv12_y_srv.Get();
    out.nv12_uv_srv = resources.sw_nv12_uv_srv.Get();
    return out.nv12_y_srv && out.nv12_uv_srv;
}

bool D3D11FramePresenter::prepare_software_planar_yuv_frame(size_t slot,
                                                            const TextureFrame& frame,
                                                            D3D11PreparedFrame& out) {
    auto& resources = tracks_[slot];
    const auto* storage = frame.cpu_planar_yuv_storage();
    if (!texture_manager_ || !storage) {
        return false;
    }

    const bool is_16bit = storage->bytes_per_sample == 2;
    for (int plane = 0; plane < 3; ++plane) {
        const int w = storage->plane_widths[plane];
        const int h = storage->plane_heights[plane];
        if (!storage->planes[plane] || w <= 0 || h <= 0 ||
            storage->strides[plane] < w * storage->bytes_per_sample) {
            spdlog::error("[D3D11FramePresenter] Invalid planar YUV plane {} "
                          "(data={}, {}x{}, stride={}, bytes_per_sample={})",
                          plane, static_cast<const void*>(storage->planes[plane]),
                          w, h, storage->strides[plane], storage->bytes_per_sample);
            return false;
        }

        bool need_new_tex = !resources.sw_planar_textures[plane];
        if (resources.sw_planar_textures[plane]) {
            D3D11_TEXTURE2D_DESC existing_desc = {};
            resources.sw_planar_textures[plane]->GetDesc(&existing_desc);
            const DXGI_FORMAT expected_format = is_16bit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
            need_new_tex =
                static_cast<int>(existing_desc.Width) != w ||
                static_cast<int>(existing_desc.Height) != h ||
                existing_desc.Format != expected_format;
        }

        if (need_new_tex) {
            resources.sw_planar_srvs[plane].Reset();
            resources.sw_planar_textures[plane].Attach(
                texture_manager_->create_plane_texture(w, h, is_16bit));
            if (resources.sw_planar_textures[plane]) {
                resources.sw_planar_srvs[plane].Attach(
                    texture_manager_->create_srv(resources.sw_planar_textures[plane].Get()));
            }
        }

        if (!resources.sw_planar_textures[plane] || !resources.sw_planar_srvs[plane]) {
            return false;
        }

        if (!texture_manager_->upload_plane_data(
                resources.sw_planar_textures[plane].Get(),
                storage->planes[plane],
                w,
                h,
                storage->strides[plane],
                is_16bit)) {
            return false;
        }
    }

    resources.nv12_uv_scale_x = 1.0f;
    resources.nv12_uv_scale_y = 1.0f;
    out.nv12_y_srv = resources.sw_planar_srvs[0].Get();
    out.planar_u_srv = resources.sw_planar_srvs[1].Get();
    out.planar_v_srv = resources.sw_planar_srvs[2].Get();
    return out.nv12_y_srv && out.planar_u_srv && out.planar_v_srv;
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
