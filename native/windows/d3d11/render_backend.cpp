#include "windows/d3d11/render_backend.h"

#include "embedded_shaders.h"
#include "renderer/overlay/analysis_overlay_renderer.h"
#include "renderer/render/presentation_backend_factory.h"
#include "renderer/render/presentation_snapshot.h"
#include "renderer/render/shader_constants.h"

#include <array>
#include <chrono>
#include <cmath>
#include <dxgi.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

namespace vr {
namespace {

RendererDrawSnapshot make_d3d_source_snapshot(
    const RendererDrawSnapshot& snapshot,
    size_t source_slot,
    int width,
    int height) {
    RendererDrawSnapshot source;
    source.decision.should_present = true;
    source.decision.current_pts_us = snapshot.decision.current_pts_us;
    source.decision.frames[0] = snapshot.decision.frames[source_slot];
    source.decision.file_ids[0] = snapshot.decision.file_ids[source_slot];
    source.decision.track_generations[0] =
        snapshot.decision.track_generations[source_slot];
    source.layout.mode = LAYOUT_SIDE_BY_SIDE;
    source.layout.split_pos = 0.5f;
    source.layout.zoom_ratio = 1.0f;
    source.layout.pixel_size_mode = PIXEL_SIZE_FILL_VIEW;
    source.layout.order[0] = 0;
    source.layout.order[1] = -1;
    source.layout.order[2] = -1;
    source.layout.order[3] = -1;
    source.track_geometry[0].active = true;
    source.track_geometry[0].width = width;
    source.track_geometry[0].height = height;
    source.track_geometry[0].aspect =
        height > 0 ? static_cast<float>(width) / height : 1.0f;
    source.tracks[0] = snapshot.tracks[source_slot];
    source.tracks[0].active = true;
    source.tracks[0].video_width = width;
    source.tracks[0].video_height = height;
    source.tracks[0].video_aspect = source.track_geometry[0].aspect;
    source.target_width = width;
    source.target_height = height;
    return source;
}

} // namespace

D3D11RenderBackend::~D3D11RenderBackend() {
    shutdown();
}

namespace {

class D3D11PresentationBackendProvider final : public PresentationBackendProvider {
public:
    bool supports(RenderBackendKind kind) const override {
        return kind == RenderBackendKind::D3D11;
    }

    std::unique_ptr<PresentationBackend> create(RenderBackendKind kind) const override {
        if (!supports(kind)) {
            return nullptr;
        }
        return std::make_unique<D3D11RenderBackend>();
    }
};

}  // namespace

const PresentationBackendProvider* default_presentation_backend_provider() {
    static const D3D11PresentationBackendProvider provider;
    return &provider;
}

std::unique_ptr<PresentationBackend> create_presentation_backend(
    RenderBackendKind kind) {
    const auto* provider = default_presentation_backend_provider();
    return provider && provider->supports(kind) ? provider->create(kind) : nullptr;
}

bool D3D11RenderBackend::initialize(const PresentationBackendConfig& config) {
    shutdown();
    last_config_ = config;
    has_last_config_ = true;
    headless_ = config.headless;
    requested_output_target_ = config.output_target;
    sdr_white_level_nits_ =
        std::isfinite(config.sdr_white_level_nits) &&
                config.sdr_white_level_nits > 0.0
            ? config.sdr_white_level_nits
            : 80.0;

    if (!initialize_device(config)) {
        return false;
    }

    texture_manager_ = std::make_unique<TextureManager>(
        device_->device(), device_->context());
    frame_presenter_ = std::make_unique<D3D11FramePresenter>(
        texture_manager_.get(), device_->context());
    shader_manager_ = std::make_unique<ShaderManager>(device_->device());
    resources_ = std::make_unique<D3D11RenderResources>();

    if (!initialize_render_resources()) {
        return false;
    }
    if (config.shared_fp16_output) {
        shared_fp16_ring_ = std::make_unique<D3D11SharedFp16Ring>();
        if (!shared_fp16_ring_->initialize(
                device_->device(), device_->context(),
                config.width, config.height)) {
            shared_fp16_ring_.reset();
            fp16_fallback_reason_ = "shared-fp16-ring-initialization-failed";
        } else {
            shared_fp16_ring_->set_frame_callback(shared_fp16_callback_);
        }
    } else if (requested_output_target_ ==
               ColorOutputTarget::kWindowsLinearScRGB) {
        if (!initialize_fp16_target(config.width, config.height)) {
            disable_fp16_target("fp16-target-initialization-failed");
        }
    }
    return true;
}

bool D3D11RenderBackend::supports_swap_chain_present() const {
    return device_ && !headless_;
}

bool D3D11RenderBackend::poll_device_removed(const char* operation) {
    return device_ && device_->poll_device_removed(operation);
}

bool D3D11RenderBackend::device_lost() const {
    return device_ && device_->device_lost();
}

long D3D11RenderBackend::device_removed_reason() const {
    return device_ ? static_cast<long>(device_->device_removed_reason()) : 0;
}

void D3D11RenderBackend::wait_idle(const char* label) {
    if (headless_output_) {
        headless_output_->wait_gpu_idle(label);
    } else if (device_ && device_->context()) {
        device_->context()->Flush();
    }
}

bool D3D11RenderBackend::present_swap_chain(int sync_interval) {
    return device_ && device_->present(sync_interval);
}

void D3D11RenderBackend::reset_track(size_t slot) {
    if (frame_presenter_) {
        frame_presenter_->reset_track(slot);
    }
}

void D3D11RenderBackend::move_track(size_t from, size_t to) {
    if (frame_presenter_) {
        frame_presenter_->move_track(from, to);
    }
}

bool D3D11RenderBackend::begin_renderer_managed_headless_frame() {
    if (!headless_output_ || !resources_) {
        return false;
    }
    std::lock_guard<std::mutex> tex_lock(headless_output_->texture_mutex());
    auto* rtv = headless_output_->begin_frame_locked();
    if (!rtv) {
        return false;
    }
    resources_->cached_rtv = rtv;
    return true;
}

std::function<void()> D3D11RenderBackend::publish_renderer_managed_headless_frame(
    const char* label) {
    if (!headless_output_) {
        return {};
    }
    headless_output_->wait_gpu_idle(label);
    std::lock_guard<std::mutex> tex_lock(headless_output_->texture_mutex());
    return headless_output_->publish_frame_locked();
}

bool D3D11RenderBackend::resize_renderer_managed_headless_output(
    int width,
    int height) {
    if (!headless_output_) {
        return false;
    }
    std::lock_guard<std::mutex> tex_lock(headless_output_->texture_mutex());
    if (!headless_output_->resize_locked(width, height)) {
        return false;
    }
    resources_->cached_rtv.Reset();
    if (fp16_target_ && !fp16_target_->resize(width, height)) {
        disable_fp16_target("fp16-target-resize-failed");
    }
    if (shared_fp16_ring_ && !shared_fp16_ring_->resize(width, height)) {
        shared_fp16_ring_.reset();
        fp16_fallback_reason_ = "shared-fp16-ring-resize-failed";
    }
    return true;
}

void D3D11RenderBackend::cleanup_renderer_managed_headless_pending_buffers() {
    if (headless_output_) {
        headless_output_->cleanup_expired_pending_buffers();
    }
}

bool D3D11RenderBackend::set_renderer_managed_headless_frame_callback(
    std::function<void()> callback) {
    if (!headless_output_) {
        return false;
    }
    headless_output_->set_frame_callback(std::move(callback));
    return true;
}

bool D3D11RenderBackend::update_sdr_white_level(double nits) {
    if (!std::isfinite(nits) || nits <= 0.0) {
        return false;
    }
    sdr_white_level_nits_ = nits;
    return true;
}

bool D3D11RenderBackend::acquire_shared_texture(
    SharedTextureSnapshot& snapshot) {
    snapshot = {};
    if (!headless_output_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(headless_output_->texture_mutex());
    D3D11HeadlessOutputTextureLease lease;
    if (!headless_output_->acquire_shared_texture_locked(lease)) {
        return false;
    }

    snapshot.type = SharedTextureHandleType::D3D11SharedHandle;
    snapshot.texture = lease.texture;
    snapshot.handle = lease.handle;
    snapshot.width = lease.width;
    snapshot.height = lease.height;
    snapshot.buffer_index = lease.buffer_index;
    snapshot.buffer_generation = lease.generation;
    return true;
}

void D3D11RenderBackend::release_shared_texture(
    int buffer_index,
    uint64_t buffer_generation) {
    if (headless_output_) {
        headless_output_->release_shared_texture(buffer_index, buffer_generation);
    }
}

void D3D11RenderBackend::snapshot_memory_stats(
    RendererGpuMemoryStats& stats,
    std::array<uint64_t, kMaxTracks>& presenter_copy_texture_bytes_by_slot)
    const {
    if (frame_presenter_) {
        const auto presenter_stats = frame_presenter_->memory_stats();
        stats.presenter_texture_bytes = presenter_stats.total_estimated_bytes;
        stats.total_estimated_bytes += stats.presenter_texture_bytes;
        for (size_t i = 0;
             i < kMaxTracks && i < presenter_stats.slots.size();
             ++i) {
            presenter_copy_texture_bytes_by_slot[i] =
                presenter_stats.slots[i].render_nv12_copy_texture_bytes;
        }
    }

    if (headless_output_) {
        const auto headless_stats = headless_output_->memory_stats();
        stats.headless_output_bytes = headless_stats.estimated_bytes;
        stats.headless_width = headless_stats.width;
        stats.headless_height = headless_stats.height;
        stats.headless_buffer_count = headless_stats.buffer_count;
        stats.total_estimated_bytes += stats.headless_output_bytes;
    }
    if (fp16_target_) {
        stats.fp16_target_bytes = fp16_target_->estimated_bytes();
        stats.total_estimated_bytes += stats.fp16_target_bytes;
    }
    if (shared_fp16_ring_) {
        stats.fp16_target_bytes = shared_fp16_ring_->estimated_bytes();
        stats.total_estimated_bytes += stats.fp16_target_bytes;
    }
    if (source_cache_ring_) {
        const uint64_t source_bytes = source_cache_ring_->estimated_bytes();
        stats.total_estimated_bytes += source_bytes;
    }

    if (resources_) {
        const auto overlay_stats =
            snapshot_analysis_overlay_memory_stats(*resources_);
        stats.analysis_overlay_bytes = overlay_stats.estimated_bytes;
        stats.analysis_overlay_width = overlay_stats.width;
        stats.analysis_overlay_height = overlay_stats.height;
        if (stats.analysis_overlay_bytes > 0) {
            stats.total_estimated_bytes += stats.analysis_overlay_bytes;
        }
    }
}

PresentationBackendDiagnostics D3D11RenderBackend::diagnostics() const {
    PresentationBackendDiagnostics result;
    result.backend = name();
    result.headless = headless_;
    if (device_) {
        const auto device_diagnostics = device_->diagnostics();
        result.adapter_description = device_diagnostics.adapter_description;
        result.driver_type = device_diagnostics.driver_type;
        result.adapter_vendor_id = device_diagnostics.adapter_vendor_id;
        result.adapter_device_id = device_diagnostics.adapter_device_id;
        result.adapter_luid_high = device_diagnostics.adapter_luid_high;
        result.adapter_luid_low = device_diagnostics.adapter_luid_low;
        result.feature_level = device_diagnostics.feature_level;
        result.warp = device_diagnostics.warp;
    }
    if (headless_output_) {
        const auto output = headless_output_->memory_stats();
        result.target_format =
            output.format == DXGI_FORMAT_B8G8R8A8_UNORM
                ? "B8G8R8A8_UNORM"
                : "unknown";
        result.width = output.width;
        result.height = output.height;
        result.buffer_count = output.buffer_count;
    } else {
        result.target_format = "R8G8B8A8_UNORM";
        result.buffer_count = 2;
    }
    result.render_target_format = (fp16_target_ || shared_fp16_ring_)
        ? "R16G16B16A16_FLOAT"
        : result.target_format;
    result.render_color_space = (fp16_target_ || shared_fp16_ring_)
        ? "scRGB-linear-bt709"
        : "sdr-bt709";
    result.sdr_compatibility_pass = (fp16_target_ || shared_fp16_ring_)
        ? "source-rerender"
        : "none";
    result.fallback_reason = fp16_fallback_reason_;
    result.fp16_target_active =
        fp16_target_ != nullptr || shared_fp16_ring_ != nullptr;
    if (fp16_target_) {
        result.fp16_target_width = fp16_target_->width();
        result.fp16_target_height = fp16_target_->height();
        result.fp16_target_buffer_count = 1;
    }
    if (shared_fp16_ring_) {
        result.fp16_target_width = shared_fp16_ring_->width();
        result.fp16_target_height = shared_fp16_ring_->height();
        result.fp16_target_buffer_count = D3D11SharedFp16Ring::kBufferCount;
    }
    if (source_cache_ring_) {
        result.source_cache_active = source_cache_ring_->texture_count() > 0;
        result.source_cache_frozen_snapshot =
            source_cache_ring_->frozen_snapshot();
        result.source_cache_ring_depth = source_cache_ring_->ring_depth();
        result.source_cache_texture_count =
            static_cast<int32_t>(source_cache_ring_->texture_count());
        result.source_cache_generation = source_cache_ring_->generation();
        result.source_cache_bytes = source_cache_ring_->estimated_bytes();
        result.source_cache_publish_count = source_cache_ring_->publish_count();
        result.source_cache_presented_anchor_generation =
            source_cache_presented_anchor_generation_;
        result.source_cache_presented_anchor_frame_generation =
            source_cache_presented_anchor_frame_generation_;
        result.source_cache_presented_anchor_publish_count =
            source_cache_presented_anchor_publish_count_;
        result.source_cache_backpressure_count =
            source_cache_ring_->backpressure_count();
        result.source_cache_fallback_count =
            source_cache_ring_->fallback_count();
        result.source_cache_last_error =
            source_cache_draw_error_ != "none"
                ? source_cache_draw_error_
                : source_cache_ring_->last_error();
    }
    result.sdr_white_level_milli_nits = static_cast<int64_t>(
        std::llround(sdr_white_level_nits_ * 1000.0));
    result.sdr_white_scale_x1000 = static_cast<int64_t>(
        std::llround(sdr_white_level_nits_ / 80.0 * 1000.0));
    result.fp16_draw_count = fp16_draw_count_;
    result.sdr_compatibility_draw_count =
        sdr_compatibility_draw_count_;
    return result;
}

bool D3D11RenderBackend::capture_front_buffer(std::vector<uint8_t>& bgra,
                                              int& width,
                                              int& height) {
    bgra.clear();
    width = 0;
    height = 0;
    if (!headless_output_) {
        return false;
    }

    D3D11HeadlessOutputFrontBufferSnapshot snapshot;
    {
        std::lock_guard<std::mutex> tex_lock(headless_output_->texture_mutex());
        if (!headless_output_->snapshot_front_buffer_locked(snapshot)) {
            return false;
        }
    }
    return headless_output_->capture_front_buffer_snapshot(
        snapshot, bgra, width, height);
}

bool D3D11RenderBackend::capture_front_buffer_region(
    int x,
    int y,
    int width,
    int height,
    std::vector<uint8_t>& bgra,
    int& region_width,
    int& region_height) {
    bgra.clear();
    region_width = 0;
    region_height = 0;
    if (!headless_output_) {
        return false;
    }

    D3D11HeadlessOutputFrontBufferSnapshot snapshot;
    {
        std::lock_guard<std::mutex> tex_lock(headless_output_->texture_mutex());
        if (!headless_output_->snapshot_front_buffer_locked(snapshot)) {
            return false;
        }
    }
    return headless_output_->capture_front_buffer_region_snapshot(
        snapshot, x, y, width, height, bgra, region_width, region_height);
}

bool D3D11RenderBackend::capture_fp16_target(
    std::vector<uint16_t>& rgba_half,
    int& width,
    int& height) const {
    if (!fp16_target_) {
        rgba_half.clear();
        width = 0;
        height = 0;
        return false;
    }
    return fp16_target_->capture_rgba16f(rgba_half, width, height);
}

bool D3D11RenderBackend::acquire_shared_fp16_texture(
    SharedFp16TextureSnapshot& snapshot) {
    return shared_fp16_ring_ && shared_fp16_ring_->acquire_latest(snapshot);
}

void D3D11RenderBackend::release_shared_fp16_texture(
    int buffer_index, uint64_t ring_generation) {
    if (shared_fp16_ring_) {
        shared_fp16_ring_->release(buffer_index, ring_generation);
    }
}

void D3D11RenderBackend::set_shared_fp16_frame_callback(
    std::function<void()> callback) {
    shared_fp16_callback_ = std::move(callback);
    if (shared_fp16_ring_) {
        shared_fp16_ring_->set_frame_callback(shared_fp16_callback_);
    }
}

bool D3D11RenderBackend::recover_device_loss(
    const char* reason,
    long removed_reason) {
    if (!has_last_config_) {
        spdlog::error(
            "[WindowsDeviceRecovery] D3D11 backend has no saved config");
        return false;
    }
    spdlog::warn(
        "[WindowsDeviceRecovery] rebuilding D3D11 backend reason={} removed=0x{:08x}",
        reason ? reason : "device-loss",
        static_cast<uint32_t>(removed_reason));
    const PresentationBackendConfig config = last_config_;
    auto shared_callback = shared_fp16_callback_;
    auto source_callback = source_cache_callback_;
    shutdown();
    shared_fp16_callback_ = shared_callback;
    source_cache_callback_ = source_callback;
    const bool ok = initialize(config);
    shared_fp16_callback_ = shared_callback;
    source_cache_callback_ = source_callback;
    if (shared_fp16_ring_) {
        shared_fp16_ring_->set_frame_callback(shared_fp16_callback_);
    }
    if (source_cache_ring_) {
        source_cache_ring_->set_frame_callback(source_cache_callback_);
    }
    source_cache_draw_error_ = ok ? "source-cache-cleared-by-device-recovery"
                                  : "device-recovery-failed";
    return ok;
}

bool D3D11RenderBackend::configure_source_cache(
    const std::vector<SourceCacheTrackDescriptor>& descriptors) {
    if (!device_ || !device_->device() || !device_->context() ||
        descriptors.empty()) {
        return false;
    }
    if (!source_cache_ring_) {
        auto ring = std::make_unique<D3D11SharedSourceCacheRing>();
        if (!ring->initialize(
                device_->device(),
                device_->context(),
                descriptors)) {
            return false;
        }
        source_cache_ring_ = std::move(ring);
    } else if (!source_cache_ring_->reconfigure(descriptors)) {
        return false;
    }
    source_cache_ring_->set_frame_callback(source_cache_callback_);
    source_cache_descriptors_ = descriptors;
    source_cache_draw_error_ = "none";
    return true;
}

void D3D11RenderBackend::clear_source_cache(const char* reason) {
    if (source_cache_ring_) {
        spdlog::info(
            "[D3D11SourceCache] clear reason={}",
            reason ? reason : "unspecified");
        source_cache_ring_->clear();
    }
    source_cache_presented_anchor_generation_ = 0;
    source_cache_presented_anchor_frame_generation_ = 0;
    source_cache_descriptors_.clear();
    source_cache_draw_error_ =
        reason ? reason : "source-cache-cleared";
}

bool D3D11RenderBackend::acquire_source_cache_bundle(
    SharedSourceCacheBundleSnapshot& snapshot) {
    return source_cache_ring_ &&
           source_cache_ring_->acquire_latest(snapshot);
}

void D3D11RenderBackend::release_source_cache_bundle(
    int buffer_index, uint64_t ring_generation) {
    if (source_cache_ring_) {
        source_cache_ring_->release(buffer_index, ring_generation);
    }
}

void D3D11RenderBackend::set_source_cache_frame_callback(
    std::function<void()> callback) {
    source_cache_callback_ = std::move(callback);
    if (source_cache_ring_) {
        source_cache_ring_->set_frame_callback(source_cache_callback_);
    }
}

bool D3D11RenderBackend::initialize_fp16_target(int width, int height) {
    if (!device_ || !device_->device() || !device_->context()) {
        return false;
    }
    auto target = std::make_unique<D3D11Fp16Target>();
    if (!target->initialize(
            device_->device(), device_->context(), width, height)) {
        return false;
    }
    fp16_target_ = std::move(target);
    fp16_fallback_reason_ = "none";
    return true;
}

void D3D11RenderBackend::disable_fp16_target(const char* reason) {
    if (fp16_target_) {
        fp16_target_->shutdown();
        fp16_target_.reset();
    }
    if (fp16_fallback_reason_ == "none") {
        spdlog::warn(
            "[WindowsPresentation] disabling FP16 scRGB target: {}",
            reason ? reason : "unknown");
    }
    fp16_fallback_reason_ = reason ? reason : "unknown";
}

bool D3D11RenderBackend::prepare_draw_resources(
    const RendererDrawSnapshot& snapshot,
    const PresentationBackendDrawHooks& hooks,
    PreparedDrawResources& prepared) {
    prepared = {};
    const auto& decision = snapshot.decision;
    const D3D11FramePresenter::GpuIdleWait wait_gpu_idle =
        hooks.wait_gpu_idle
            ? hooks.wait_gpu_idle
            : D3D11FramePresenter::GpuIdleWait([](const char*) {});
    if (!frame_presenter_) {
        return true;
    }

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!decision.frames[i].has_value() ||
            !decision.frames[i]->texture_handle ||
            !snapshot.tracks[i].active ||
            decision.file_ids[i] != snapshot.tracks[i].file_id ||
            decision.track_generations[i] != snapshot.tracks[i].generation) {
            continue;
        }

        const auto prepare_start = std::chrono::steady_clock::now();
        const bool prepared_ok = frame_presenter_->prepare_frame(
            i,
            decision.frames[i].value(),
            snapshot.target_width,
            snapshot.target_height,
            wait_gpu_idle,
            prepared.frames[i]);
        if (hooks.record_frame_copy_us) {
            hooks.record_frame_copy_us(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - prepare_start).count()));
        }
        if (!prepared_ok) {
            continue;
        }

        prepared.rgba_srvs[i] = prepared.frames[i].rgba_srv;
        prepared.y_srvs[i] = prepared.frames[i].nv12_y_srv;
        prepared.uv_srvs[i] = prepared.frames[i].nv12_uv_srv;
        prepared.u_srvs[i] = prepared.frames[i].planar_u_srv;
        prepared.v_srvs[i] = prepared.frames[i].planar_v_srv;
    }
    return true;
}

bool D3D11RenderBackend::draw_prepared_pass(
    const RendererDrawSnapshot& snapshot,
    const PresentationBackendDrawHooks& hooks,
    const PreparedDrawResources& prepared,
    ID3D11RenderTargetView* target_rtv,
    ColorOutputTarget output_target,
    bool draw_overlay) {
    if (!resources_ || !device_ || !target_rtv) {
        return false;
    }
    const auto& decision = snapshot.decision;
    auto& resources = *resources_;
    auto* ctx = device_->context();

    const float clear_color[4] = {
        snapshot.background_color[0],
        snapshot.background_color[1],
        snapshot.background_color[2],
        snapshot.background_color[3],
    };
    ctx->ClearRenderTargetView(target_rtv, clear_color);
    ctx->OMSetRenderTargets(1, &target_rtv, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(snapshot.target_width);
    vp.Height = static_cast<float>(snapshot.target_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    ID3D11Buffer* vb = resources.vertex_buffer.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (resources.compiled_shader.layout) {
        ctx->IASetInputLayout(resources.compiled_shader.layout.Get());
    }

    ctx->VSSetShader(resources.compiled_shader.vs.Get(), nullptr, 0);
    ctx->PSSetShader(resources.compiled_shader.ps.Get(), nullptr, 0);

    const auto presentation = build_presentation_snapshot(
        decision,
        snapshot.layout,
        snapshot.track_geometry,
        snapshot.target_width,
        snapshot.target_height,
        snapshot.background_color);
    ShaderConstants cb = presentation.constants;
    bool constants_ready = false;
    if (resources.compiled_shader.constant_buffer) {
        cb.nv12_mask = 0;
        cb.planar_yuv_mask = 0;
        cb.output_target =
            output_target == ColorOutputTarget::kWindowsLinearScRGB ? 1 : 0;
        cb.sdr_white_scale =
            static_cast<float>(sdr_white_level_nits_ / 80.0);

        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!snapshot.tracks[i].active) {
                cb.nv12_uv_scale_x[i] = 1.0f;
                cb.nv12_uv_scale_y[i] = 1.0f;
                cb.color_range[i] = VIDEO_COLOR_RANGE_LIMITED;
                cb.color_matrix[i] = VIDEO_COLOR_MATRIX_BT709;
                cb.color_transfer[i] = VIDEO_COLOR_TRANSFER_SDR;
                cb.color_primaries[i] = VIDEO_COLOR_PRIMARIES_BT709;
                continue;
            }
            const bool frame_matches_track =
                decision.frames[i].has_value() &&
                decision.file_ids[i] == snapshot.tracks[i].file_id &&
                decision.track_generations[i] == snapshot.tracks[i].generation;
            if (!frame_matches_track) {
                cb.nv12_uv_scale_x[i] = 1.0f;
                cb.nv12_uv_scale_y[i] = 1.0f;
                continue;
            }
            if (decision.frames[i]->cpu_planar_yuv_storage()) {
                cb.planar_yuv_mask |= (1 << static_cast<int>(i));
                cb.nv12_uv_scale_x[i] = 1.0f;
                cb.nv12_uv_scale_y[i] = 1.0f;
            } else if (decision.frames[i]->is_nv12) {
                cb.nv12_mask |= (1 << static_cast<int>(i));
                cb.nv12_uv_scale_x[i] =
                    prepared.frames[i].nv12_uv_scale_x;
                cb.nv12_uv_scale_y[i] =
                    prepared.frames[i].nv12_uv_scale_y;
            } else {
                cb.nv12_uv_scale_x[i] =
                    prepared.frames[i].nv12_uv_scale_x;
                cb.nv12_uv_scale_y[i] =
                    prepared.frames[i].nv12_uv_scale_y;
            }
        }
        ctx->UpdateSubresource(
            resources.compiled_shader.constant_buffer.Get(),
            0,
            nullptr,
            &cb,
            0,
            0);
        ctx->VSSetConstantBuffers(
            0, 1, resources.compiled_shader.constant_buffer.GetAddressOf());
        ctx->PSSetConstantBuffers(
            0, 1, resources.compiled_shader.constant_buffer.GetAddressOf());
        constants_ready = true;
    }

    if (resources.sampler_state) {
        ID3D11SamplerState* sampler = resources.sampler_state.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
    }
    ctx->PSSetShaderResources(
        0, static_cast<UINT>(prepared.rgba_srvs.size()),
        prepared.rgba_srvs.data());
    ctx->PSSetShaderResources(
        4, static_cast<UINT>(prepared.y_srvs.size()),
        prepared.y_srvs.data());
    ctx->PSSetShaderResources(
        8, static_cast<UINT>(prepared.uv_srvs.size()),
        prepared.uv_srvs.data());
    ctx->PSSetShaderResources(
        12, static_cast<UINT>(prepared.u_srvs.size()),
        prepared.u_srvs.data());
    ctx->PSSetShaderResources(
        16, static_cast<UINT>(prepared.v_srvs.size()),
        prepared.v_srvs.data());
    ctx->Draw(4, 0);

    if (constants_ready && draw_overlay && hooks.draw_overlay) {
        hooks.draw_overlay(*this, snapshot);
    }

    std::array<ID3D11ShaderResourceView*, kMaxTracks> null_srvs{};
    ctx->PSSetShaderResources(
        0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    ctx->PSSetShaderResources(
        4, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    ctx->PSSetShaderResources(
        8, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    ctx->PSSetShaderResources(
        12, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    ctx->PSSetShaderResources(
        16, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    return true;
}

bool D3D11RenderBackend::draw_source_cache_bundle(
    const RendererDrawSnapshot& snapshot,
    const PresentationBackendDrawHooks& hooks,
    const PreparedDrawResources& prepared) {
    if (!source_cache_ring_ || source_cache_descriptors_.empty()) {
        return false;
    }
    const auto report_failure = [this](const std::string& error) {
        if (source_cache_draw_error_ != error) {
            source_cache_draw_error_ = error;
            spdlog::warn("[D3D11SourceCache] {}", error);
        }
    };
    std::array<ID3D11RenderTargetView*, 4> rtvs{};
    size_t target_count = 0;
    if (!source_cache_ring_->begin_bundle(rtvs, target_count)) {
        return false;
    }
    if (target_count != source_cache_descriptors_.size()) {
        report_failure("bundle-target-count-mismatch");
        source_cache_ring_->cancel_bundle();
        return false;
    }

    bool complete = true;
    for (size_t target = 0; target < target_count; ++target) {
        const auto& descriptor = source_cache_descriptors_[target];
        const size_t source_slot = static_cast<size_t>(descriptor.slot);
        if (source_slot >= kMaxTracks ||
            !snapshot.decision.frames[source_slot].has_value() ||
            !snapshot.tracks[source_slot].active ||
            snapshot.decision.file_ids[source_slot] != descriptor.file_id ||
            snapshot.tracks[source_slot].file_id != descriptor.file_id ||
            snapshot.decision.frames[source_slot]->width != descriptor.width ||
            snapshot.decision.frames[source_slot]->height != descriptor.height) {
            const auto* frame =
                source_slot < kMaxTracks &&
                        snapshot.decision.frames[source_slot].has_value()
                    ? &snapshot.decision.frames[source_slot].value()
                    : nullptr;
            report_failure(
                "track-not-ready slot=" +
                std::to_string(descriptor.slot) +
                " file=" + std::to_string(descriptor.file_id) +
                " decisionFile=" +
                std::to_string(
                    source_slot < kMaxTracks
                        ? snapshot.decision.file_ids[source_slot]
                        : -1) +
                " active=" +
                std::to_string(
                    source_slot < kMaxTracks &&
                    snapshot.tracks[source_slot].active) +
                " frame=" +
                (frame
                    ? std::to_string(frame->width) + "x" +
                          std::to_string(frame->height)
                    : "missing") +
                " expected=" + std::to_string(descriptor.width) + "x" +
                std::to_string(descriptor.height));
            complete = false;
            break;
        }
        auto source_snapshot = make_d3d_source_snapshot(
            snapshot,
            source_slot,
            descriptor.width,
            descriptor.height);
        PreparedDrawResources source_prepared;
        source_prepared.frames[0] = prepared.frames[source_slot];
        source_prepared.rgba_srvs[0] = prepared.rgba_srvs[source_slot];
        source_prepared.y_srvs[0] = prepared.y_srvs[source_slot];
        source_prepared.uv_srvs[0] = prepared.uv_srvs[source_slot];
        source_prepared.u_srvs[0] = prepared.u_srvs[source_slot];
        source_prepared.v_srvs[0] = prepared.v_srvs[source_slot];
        if (!draw_prepared_pass(
                source_snapshot,
                hooks,
                source_prepared,
                rtvs[target],
                ColorOutputTarget::kWindowsLinearScRGB,
                false)) {
            report_failure(
                "source-pass-draw-failed slot=" +
                std::to_string(descriptor.slot));
            complete = false;
            break;
        }
    }
    if (!complete) {
        source_cache_ring_->cancel_bundle();
        return false;
    }
    device_->context()->Flush();
    std::shared_ptr<const AnalysisOverlayPrimitivePackage> overlay;
    if (hooks.build_overlay_primitives) {
        overlay = hooks.build_overlay_primitives(snapshot);
    }
    SourceCachePublishInfo publish_info;
    if (!source_cache_ring_->publish_bundle(std::move(overlay), &publish_info)) {
        report_failure("bundle-publish-failed");
        return false;
    }
    source_cache_presented_anchor_generation_ =
        publish_info.ring_generation;
    source_cache_presented_anchor_frame_generation_ =
        publish_info.frame_generation;
    ++source_cache_presented_anchor_publish_count_;
    source_cache_draw_error_ = "none";
    return true;
}

bool D3D11RenderBackend::initialize_device(const D3D11RenderBackendConfig& config) {
    device_ = std::make_unique<D3D11Device>();
    if (config.headless) {
        auto* adapter = static_cast<IDXGIAdapter*>(config.adapter);
        if (!device_->initialize_headless(adapter, config.width, config.height)) {
            spdlog::error("Renderer: failed to initialize D3D11 device (headless)");
            return false;
        }
        headless_output_ = std::make_unique<D3D11HeadlessOutput>();
        if (!headless_output_->initialize(
                device_->device(), device_->context(), config.width, config.height)) {
            return false;
        }
        return true;
    }

    if (!device_->initialize(config.hwnd, config.width, config.height)) {
        spdlog::error("Renderer: failed to initialize D3D11 device");
        return false;
    }
    return true;
}

bool D3D11RenderBackend::initialize_render_resources() {
    const std::vector<EmbeddedShaderFile> multitrack_includes = {
        {"common.hlsl", kCommonHlsl, sizeof(kCommonHlsl) - 1},
        {"color_pipeline.hlsl", kColorPipelineHlsl, sizeof(kColorPipelineHlsl) - 1},
        {"sampling.hlsl", kSamplingHlsl, sizeof(kSamplingHlsl) - 1},
    };
    if (!shader_manager_->compile_from_source(
            kMultitrackHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->compiled_shader)) {
        spdlog::error("Renderer: failed to compile shaders");
        return false;
    }

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_shader)) {
        spdlog::error("Renderer: failed to compile overlay shaders");
        return false;
    }

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayInvertHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_invert_shader)) {
        spdlog::error("Renderer: failed to compile overlay invert shaders");
        return false;
    }

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayContrastHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_contrast_shader)) {
        spdlog::error("Renderer: failed to compile overlay contrast shaders");
        return false;
    }

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayRectHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_rect_shader)) {
        spdlog::error("Renderer: failed to compile overlay rect shaders");
        return false;
    }

    if (!shader_manager_->compile_from_source(
            kAnalysisOverlayMaskRectHlsl,
            multitrack_includes,
            "VSMain",
            "PSMain",
            resources_->overlay_mask_rect_shader)) {
        spdlog::error("Renderer: failed to compile overlay mask rect shaders");
        return false;
    }

    if (!shader_manager_->create_constant_buffer(
            device_->device(),
            static_cast<UINT>(kShaderConstantsSize),
            resources_->compiled_shader)) {
        spdlog::error("Renderer: failed to create constant buffer");
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT hr = device_->device()->CreateSamplerState(
        &sampler_desc, &resources_->sampler_state);
    if (FAILED(hr) || !resources_->sampler_state) {
        spdlog::error("Renderer: CreateSamplerState failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_SAMPLER_DESC overlay_sampler_desc = sampler_desc;
    overlay_sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = device_->device()->CreateSamplerState(
        &overlay_sampler_desc, &resources_->overlay_sampler_state);
    if (FAILED(hr) || !resources_->overlay_sampler_state) {
        spdlog::error("Renderer: CreateSamplerState(overlay) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->device()->CreateBlendState(
        &blend_desc, &resources_->overlay_blend_state);
    if (FAILED(hr) || !resources_->overlay_blend_state) {
        spdlog::error("Renderer: CreateBlendState(overlay) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    D3D11_BLEND_DESC invert_blend_desc = {};
    invert_blend_desc.RenderTarget[0].BlendEnable = TRUE;
    invert_blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    invert_blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    invert_blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_SUBTRACT;
    invert_blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    invert_blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    invert_blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    invert_blend_desc.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED |
        D3D11_COLOR_WRITE_ENABLE_GREEN |
        D3D11_COLOR_WRITE_ENABLE_BLUE;
    hr = device_->device()->CreateBlendState(
        &invert_blend_desc, &resources_->overlay_invert_blend_state);
    if (FAILED(hr) || !resources_->overlay_invert_blend_state) {
        spdlog::error("Renderer: CreateBlendState(overlay invert) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    struct Vertex { float x, y, u, v; };
    Vertex quad[] = {
        {-1, -1, 0, 1},
        {-1,  1, 0, 0},
        { 1, -1, 1, 1},
        { 1,  1, 1, 0},
    };
    D3D11_BUFFER_DESC vb_desc = {};
    vb_desc.ByteWidth = sizeof(quad);
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA vb_data = {};
    vb_data.pSysMem = quad;
    hr = device_->device()->CreateBuffer(
        &vb_desc, &vb_data, &resources_->vertex_buffer);
    if (FAILED(hr) || !resources_->vertex_buffer) {
        spdlog::error("Renderer: CreateBuffer(vertex) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    return true;
}

bool D3D11RenderBackend::draw_frame(const RendererDrawSnapshot& snapshot,
                                    const PresentationBackendDrawHooks& hooks) {
    if (!resources_ || !device_) {
        return false;
    }
    auto& resources = *resources_;

    if (!resources.cached_rtv) {
        if (!headless_) {
            ID3D11Texture2D* back_buffer = nullptr;
            HRESULT hr = device_->swap_chain()->GetBuffer(
                0,
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&back_buffer));
            if (FAILED(hr)) {
                spdlog::error("[Renderer] Failed to get back buffer: HRESULT {:#x}",
                              static_cast<unsigned long>(hr));
                return false;
            }
            hr = device_->device()->CreateRenderTargetView(
                back_buffer, nullptr, &resources.cached_rtv);
            back_buffer->Release();
            if (FAILED(hr)) {
                spdlog::error("[Renderer] Failed to create RTV: HRESULT {:#x}",
                              static_cast<unsigned long>(hr));
                return false;
            }
        }
    }

    if (!resources.cached_rtv) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> sdr_rtv =
        resources.cached_rtv;
    PreparedDrawResources prepared;
    if (!prepare_draw_resources(snapshot, hooks, prepared)) {
        return false;
    }

    (void)draw_source_cache_bundle(snapshot, hooks, prepared);

    bool fp16_drawn = false;
    if (fp16_target_) {
        fp16_drawn = draw_prepared_pass(
            snapshot,
            hooks,
            prepared,
            fp16_target_->rtv(),
            ColorOutputTarget::kWindowsLinearScRGB,
            true);
        if (fp16_drawn) {
            ++fp16_draw_count_;
        } else if (!device_lost()) {
            disable_fp16_target("fp16-draw-failed");
        }
    }
    if (shared_fp16_ring_) {
        auto* shared_rtv = shared_fp16_ring_->begin_frame();
        if (shared_rtv) {
            fp16_drawn = draw_prepared_pass(
                snapshot, hooks, prepared, shared_rtv,
                ColorOutputTarget::kWindowsLinearScRGB, true);
            if (fp16_drawn) {
                device_->context()->Flush();
                if (!shared_fp16_ring_->publish_frame()) {
                    fp16_drawn = false;
                }
            } else {
                shared_fp16_ring_->cancel_frame();
            }
            if (fp16_drawn) {
                ++fp16_draw_count_;
            }
        }
    }

    const bool sdr_drawn = draw_prepared_pass(
        snapshot,
        hooks,
        prepared,
        sdr_rtv.Get(),
        ColorOutputTarget::kSDRToneMappedBT709,
        true);
    resources.cached_rtv = sdr_rtv;
    if (sdr_drawn && fp16_drawn) {
        ++sdr_compatibility_draw_count_;
    }
    return sdr_drawn;
}

void D3D11RenderBackend::shutdown() {
    clear_source_cache("backend-shutdown");
    if (shared_fp16_ring_) {
        shared_fp16_ring_->shutdown();
        shared_fp16_ring_.reset();
    }
    if (fp16_target_) {
        fp16_target_->shutdown();
        fp16_target_.reset();
    }
    shader_manager_.reset();
    frame_presenter_.reset();
    texture_manager_.reset();
    resources_.reset();
    headless_output_.reset();

    if (device_) {
        device_->shutdown();
        device_.reset();
    }
    headless_ = false;
    requested_output_target_ = ColorOutputTarget::kSDRToneMappedBT709;
    sdr_white_level_nits_ = 80.0;
    fp16_draw_count_ = 0;
    sdr_compatibility_draw_count_ = 0;
    fp16_fallback_reason_ = "none";
    source_cache_presented_anchor_generation_ = 0;
    source_cache_presented_anchor_frame_generation_ = 0;
    source_cache_presented_anchor_publish_count_ = 0;
    source_cache_callback_ = {};
}

} // namespace vr
