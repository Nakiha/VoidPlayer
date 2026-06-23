#include "windows_native_compositor.h"

#include "windows/presentation/windows_dcomp_composite.h"
#include "renderer/overlay/analysis_overlay_primitives.h"

#include <d3dcompiler.h>
#include <d2d1_1.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace {

constexpr int kExportDisabled = 0;
constexpr int kExportMirror = 1;
constexpr int kExportCompositorOwned = 2;
constexpr auto kFlutterExportStaleTimeout = std::chrono::milliseconds(750);
constexpr int64_t kMinUnrequestedFlutterExportSignalUs = 1000;
constexpr int64_t kMaxUnrequestedFlutterExportSignalUs = 16667;
constexpr int64_t kMinRetainedDeferredContentDelayUs = 1000;
constexpr int64_t kMaxRetainedDeferredContentDelayUs = 16667;
constexpr int64_t kMinRetainedGraphCommitIntervalUs = 1000;
constexpr int64_t kMaxRetainedGraphCommitIntervalUs = 16667;
constexpr auto kRetainedProjectionInteractionWindow =
    std::chrono::milliseconds(50);
constexpr auto kSourceProjectionDebugSampleInterval =
    std::chrono::milliseconds(250);
constexpr auto kFlutterExportPacingSampleInterval =
    std::chrono::milliseconds(250);
constexpr size_t kMaxRetainedGraphTimingSamples = 512;

struct CompositeConstants {
    float viewport[4];
    float sdr_white_scale;
    float output_mode;
    float source_projection_enabled;
    float source_mode;
    float source_split_pos;
    float source_track_count;
    float source_header_padding[2];
    float source_present[4];
    float source_order[4];
    float source_transfer[4];
    float source_display_offset_x[4];
    float source_display_offset_y[4];
    float source_inv_display_size_x[4];
    float source_inv_display_size_y[4];
    float source_view_offset_uv_x[4];
    float source_view_offset_uv_y[4];
    float background_color[4];
};

struct OverlayVertex {
    float u = 0.0f;
    float v = 0.0f;
    float source_slot = 0.0f;
    float padding = 0.0f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

float srgb_to_linear(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.04045f
        ? value / 12.92f
        : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

bool adapter_luid(IDXGIAdapter* adapter, int32_t& high, uint32_t& low) {
    high = 0;
    low = 0;
    if (!adapter) {
        return false;
    }
    DXGI_ADAPTER_DESC desc = {};
    if (FAILED(adapter->GetDesc(&desc))) {
        return false;
    }
    high = desc.AdapterLuid.HighPart;
    low = desc.AdapterLuid.LowPart;
    return true;
}

DXGI_FORMAT retained_surface_format(
    WindowsNativeCompositor::OutputTarget target) {
    return target == WindowsNativeCompositor::OutputTarget::ScRGB
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_B8G8R8A8_UNORM;
}

void append_retained_graph_sample(std::vector<int64_t>& samples,
                                  int64_t value) {
    if (value < 0) {
        return;
    }
    if (samples.size() >= kMaxRetainedGraphTimingSamples) {
        samples.erase(samples.begin());
    }
    samples.push_back(value);
}

int64_t retained_graph_p95(std::vector<int64_t> samples) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const auto index = static_cast<size_t>(
        std::ceil(static_cast<double>(samples.size()) * 0.95) - 1.0);
    return samples[std::min(index, samples.size() - 1)];
}

int64_t retained_graph_commit_interval_us(int64_t display_hz,
                                          bool projection_only) {
    display_hz = std::max<int64_t>(1, display_hz);
    const int64_t full_frame_us = std::clamp<int64_t>(
        1000000 / display_hz,
        kMinRetainedGraphCommitIntervalUs,
        kMaxRetainedGraphCommitIntervalUs);
    if (!projection_only) {
        return full_frame_us;
    }
    return std::clamp<int64_t>(
        full_frame_us / 2,
        kMinRetainedGraphCommitIntervalUs,
        kMaxRetainedGraphCommitIntervalUs);
}

uint64_t steady_micros() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

int64_t age_ms(uint64_t now_us, uint64_t then_us) {
    if (then_us == 0 || now_us < then_us) {
        return -1;
    }
    return static_cast<int64_t>((now_us - then_us) / 1000);
}

uint64_t counter_delta(uint64_t current, uint64_t previous) {
    return current >= previous ? current - previous : current;
}

std::string luid_string(int32_t high, uint32_t low) {
    return std::to_string(high) + ":" + std::to_string(low);
}

} // namespace

WindowsNativeCompositor::WindowsNativeCompositor() = default;

WindowsNativeCompositor::~WindowsNativeCompositor() {
    Stop();
}

void WindowsNativeCompositor::ResetRetainedGraph(const std::string& reason) {
    retained_background_ = {};
    retained_flutter_ = {};
    for (auto& source : retained_sources_) {
        source = {};
    }
    retained_source_root_visual_.Reset();
    retained_background_visual_.Reset();
    retained_root_visual_.Reset();
    retained_graph_active_ = false;
    retained_projection_dirty_ = false;
    retained_source_content_dirty_ = false;
    retained_flutter_content_dirty_ = false;
    retained_deferred_content_deadline_ = {};
    retained_graph_commit_deadline_ = {};
    last_retained_graph_commit_time_ = {};
    last_retained_projection_update_ = {};
    retained_graph_fallback_reason_ =
        reason.empty() ? "reset" : reason;
}

bool WindowsNativeCompositor::CanUseRetainedGraph(
    const SourceProjection& projection,
    OutputTarget target) const {
    return projection.enabled &&
           (target == OutputTarget::SDR || target == OutputTarget::ScRGB) &&
           !IsCrossAdapterActive() &&
           dcomp_device_ &&
           device_ &&
           context_;
}

bool WindowsNativeCompositor::Start(
    HWND hwnd,
    void* flutter_view,
    const std::shared_ptr<vr::NativePlayer>& player,
    IDXGIAdapter* producer_adapter,
    IDXGIAdapter* output_adapter,
    double sdr_white_level_nits,
    OutputTarget output_target,
    StateCallback callback) {
    Stop();
    hwnd_ = hwnd;
    flutter_view_ = flutter_view;
    player_ = player;
    state_callback_ = std::move(callback);
    sdr_white_scale_ =
        std::isfinite(sdr_white_level_nits) && sdr_white_level_nits > 0.0
            ? sdr_white_level_nits / 80.0
            : 1.0;
    desired_output_target_ = output_target;
    cross_adapter_sync_request_ =
        vr::parse_windows_cross_adapter_sync_request(
            std::getenv("VOIDPLAYER_WINDOWS_CROSS_ADAPTER_SYNC"));
    const bool engine_api_available = LoadEngineApi();
    const bool frame_pump_available = engine_api_.frame_pump_available();
    if (!hwnd_ || !flutter_view_ || !player || !engine_api_available ||
        !frame_pump_available) {
        spdlog::error(
            "[WindowsNativeCompositor] surface export unavailable: "
            "hwnd={} flutter_view={} player={} engine_api={} frame_pump={}",
            hwnd_ != nullptr,
            flutter_view_ != nullptr,
            static_cast<bool>(player),
            engine_api_available,
            frame_pump_available);
        diagnostics_.fallback_reason =
            frame_pump_available
                ? "flutter-surface-export-unavailable"
                : "flutter-surface-export-frame-pump-unavailable";
        return false;
    }
    if (!InitializeDeviceAndComposition(
            producer_adapter,
            output_adapter ? output_adapter : producer_adapter) ||
        !CreatePipeline()) {
        diagnostics_.fallback_reason = "dcomp-initialization-failed";
        return false;
    }

    diagnostics_.engine_export_available = true;
    diagnostics_.engine_export_frame_pump_available = true;
    engine_api_.set_callback(
        flutter_view_, OnFlutterSurfacePublished, this);
    if (!engine_api_.set_mode(flutter_view_, kExportMirror)) {
        diagnostics_.fallback_reason = "flutter-export-mirror-failed";
        return false;
    }
    player->set_shared_fp16_frame_callback([this]() { SignalWork(); });
    player->set_source_cache_frame_callback([this]() { SignalWork(); });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
        work_pending_ = true;
        terminal_inactive_ = false;
        rate_start_time_ = std::chrono::steady_clock::now();
        source_cache_publish_count_ = 0;
        source_cache_base_lease_wait_logged_ = false;
        source_cache_bundle_acquire_logged_ = false;
        source_cache_consumed_logged_ = false;
        flutter_export_unsolicited_signal_count_ = 0;
        flutter_export_unsolicited_throttle_count_ = 0;
        last_unsolicited_flutter_export_signal_ = {};
        last_explicit_flutter_frame_request_time_ = {};
        high_refresh_metrics_.reset(diagnostics_.high_refresh_display_hz);
        retained_graph_flutter_bake_us_.clear();
        retained_graph_source_bake_us_.clear();
        retained_graph_apply_us_.clear();
        retained_graph_commit_us_.clear();
        retained_graph_commit_deadline_ = {};
        last_retained_graph_commit_time_ = {};
        last_present_time_ = {};
        interaction_sample_started_ = {};
        last_overlay_metrics_generation_ = 0;
        interaction_sample_active_ = false;
        diagnostics_.desired_output_target =
            OutputTargetName(output_target);
        diagnostics_.transition_reason = "initial";
        diagnostics_.producer_adapter_luid =
            luid_string(producer_luid_high_, producer_luid_low_);
        diagnostics_.output_adapter_luid =
            luid_string(output_luid_high_, output_luid_low_);
        diagnostics_.pending_output_adapter_luid =
            diagnostics_.output_adapter_luid;
        diagnostics_.cross_adapter_required = IsCrossAdapterActive();
        diagnostics_.cross_adapter_transport_mode =
            IsCrossAdapterActive() ? "row-major-gpu-copy" : "same-adapter";
        diagnostics_.cross_adapter_transport_status =
            IsCrossAdapterActive() ? transport_support_.status
                                   : "not-required";
        diagnostics_.cross_adapter_sync_kind =
            IsCrossAdapterActive() ? "event-query"
                                   : "keyed-mutex";
        diagnostics_.cross_adapter_requested_sync_kind =
            vr::windows_cross_adapter_sync_request_name(
                cross_adapter_sync_request_);
        diagnostics_.cross_adapter_active_sync_kind =
            diagnostics_.cross_adapter_sync_kind;
        diagnostics_.cross_adapter_sync_fallback_reason = "none";
        diagnostics_.cross_adapter_supported =
            !IsCrossAdapterActive() || transport_support_.bgra8;
        diagnostics_.transport_bgra8_supported = transport_support_.bgra8;
        diagnostics_.transport_fp16_supported = transport_support_.rgba16f;
        diagnostics_.transport_shared_fence_supported =
            transport_support_.shared_fence;
        diagnostics_.transport_shared_fence_producer_supported =
            transport_support_.shared_fence_producer;
        diagnostics_.transport_shared_fence_output_supported =
            transport_support_.shared_fence_output;
        diagnostics_.transport_shared_fence_handle_created =
            transport_support_.shared_fence_handle_created;
        diagnostics_.transport_shared_fence_open_succeeded =
            transport_support_.shared_fence_open_succeeded;
    }
    thread_ = std::thread(&WindowsNativeCompositor::ThreadMain, this);
    return true;
}

void WindowsNativeCompositor::Stop(const char* reason) {
    const bool had_state =
        thread_.joinable() || flutter_view_ != nullptr ||
        static_cast<bool>(state_callback_);
    if (had_state) {
        spdlog::info(
            "[WindowsNativeCompositor] stop begin reason={}",
            reason ? reason : "shutdown");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        work_pending_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        spdlog::info(
            "[WindowsNativeCompositor] waiting for composition thread");
        thread_.join();
        spdlog::info(
            "[WindowsNativeCompositor] composition thread joined");
    }
    if (auto player = player_.lock()) {
        ReleaseHeldInputs(player);
        player->set_shared_fp16_frame_callback({});
        player->set_source_cache_frame_callback({});
        player->clear_source_cache(reason ? reason : "compositor-stop");
    }
    if (engine_api_.available() && flutter_view_) {
        engine_api_.set_callback(flutter_view_, nullptr, nullptr);
        engine_api_.set_mode(flutter_view_, kExportDisabled);
    }
    if (dcomp_target_) dcomp_target_->SetRoot(nullptr);
    if (dcomp_device_) dcomp_device_->Commit();
    pending_swap_chain_ = {};
    current_swap_chain_ = {};
    ResetRetainedGraph("stop");
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();
    constants_.Reset();
    sampler_.Reset();
    pixel_shader_.Reset();
    video_pixel_shader_.Reset();
    flutter_pixel_shader_.Reset();
    premultiplied_blend_state_.Reset();
    overlay_blend_state_.Reset();
    overlay_input_layout_.Reset();
    overlay_pixel_shader_.Reset();
    overlay_vertex_shader_.Reset();
    ResetOverlayLayer(reason ? reason : "shutdown");
    vertex_shader_.Reset();
    context_.Reset();
    device_.Reset();
    producer_context_.Reset();
    producer_device_.Reset();
    producer_adapter_.Reset();
    output_adapter_.Reset();
    pending_output_adapter_.Reset();
    video_transport_.reset();
    sdr_video_transport_.reset();
    flutter_transport_.reset();
    for (auto& transport : source_transports_) {
        transport.reset();
    }
    flutter_view_ = nullptr;
    hwnd_ = nullptr;
    player_.reset();
    if (had_state) {
        PublishState(Phase::Inactive, reason ? reason : "shutdown");
    }
    state_callback_ = {};
}

void WindowsNativeCompositor::ReleaseHeldInputs(
    const std::shared_ptr<vr::NativePlayer>& player) {
    if (held_source_valid_) {
        for (size_t slot = 0; slot < held_source_mutexes_.size(); ++slot) {
            if (held_source_present_[slot] && held_source_mutexes_[slot]) {
                held_source_mutexes_[slot]->ReleaseSync(0);
            }
        }
        if (player && held_source_.buffer_index >= 0) {
            player->release_source_cache_bundle(
                held_source_.buffer_index,
                held_source_.ring_generation);
        }
    }
    held_source_valid_ = false;
    held_source_ = {};
    held_source_present_.fill(false);
    held_source_transfer_.fill(0);
    held_source_srvs_ = {};
    held_source_mutexes_ = {};
    held_source_textures_ = {};
    ResetOverlayLayer("source-cache-release");

    if (held_flutter_valid_) {
        if (held_flutter_mutex_) {
            held_flutter_mutex_->ReleaseSync(
                held_flutter_.producer_release_key);
        }
        if (engine_api_.available() && flutter_view_ &&
            held_flutter_.lease_id != 0) {
            engine_api_.release(flutter_view_, held_flutter_.lease_id);
        }
    }
    held_flutter_valid_ = false;
    held_flutter_ = {};
    held_flutter_srv_.Reset();
    held_flutter_mutex_.Reset();
    held_flutter_texture_.Reset();

    if (held_video_valid_) {
        if (held_video_mutex_) {
            held_video_mutex_->ReleaseSync(
                held_video_.producer_release_key);
        }
        if (player && held_video_.buffer_index >= 0) {
            player->release_shared_fp16_texture(
                held_video_.buffer_index,
                held_video_.ring_generation);
        }
    }
    held_video_valid_ = false;
    held_video_ = {};
    held_video_srv_.Reset();
    held_video_mutex_.Reset();
    held_video_texture_.Reset();

    if (held_sdr_video_valid_) {
        if (player && held_sdr_video_.buffer_index >= 0) {
            player->release_shared_texture(
                held_sdr_video_.buffer_index,
                held_sdr_video_.buffer_generation);
        }
        if (held_sdr_video_.texture) {
            static_cast<ID3D11Texture2D*>(
                held_sdr_video_.texture)->Release();
        }
    }
    held_sdr_video_valid_ = false;
    held_sdr_video_ = {};
    held_sdr_video_srv_.Reset();
    held_sdr_video_texture_.Reset();
}

void WindowsNativeCompositor::ResetOverlayLayer(const std::string& reason) {
    overlay_vertex_buffer_.Reset();
    overlay_vertex_count_ = 0;
    overlay_layer_signature_ = {};
    std::lock_guard<std::mutex> lock(mutex_);
    overlay_layer_state_.reset(reason);
    const auto overlay_state = overlay_layer_state_.snapshot();
    diagnostics_.overlay_retained_layer_active = overlay_state.active;
    diagnostics_.overlay_layer_mode = overlay_state.mode;
    diagnostics_.overlay_layer_texture_count = overlay_state.texture_count;
    diagnostics_.overlay_layer_bytes = overlay_state.bytes;
    diagnostics_.overlay_layer_generation = overlay_state.generation;
    diagnostics_.overlay_layer_committed_generation =
        overlay_state.committed_generation;
    diagnostics_.overlay_layer_pending_generation =
        overlay_state.pending_generation;
    diagnostics_.overlay_layer_composite_count = overlay_state.composite_count;
    diagnostics_.overlay_layer_miss_count = overlay_state.miss_count;
    diagnostics_.overlay_layer_backpressure_count =
        overlay_state.backpressure_count;
    diagnostics_.overlay_layer_fallback_reason =
        overlay_state.fallback_reason;
    diagnostics_.overlay_layer_last_error = overlay_state.last_error;
}

void WindowsNativeCompositor::SetViewportRect(
    double left, double top, double right, double bottom) {
    std::lock_guard<std::mutex> lock(mutex_);
    viewport_[0] = std::clamp(left, 0.0, 1.0);
    viewport_[1] = std::clamp(top, 0.0, 1.0);
    viewport_[2] = std::clamp(right, viewport_[0], 1.0);
    viewport_[3] = std::clamp(bottom, viewport_[1], 1.0);
    const bool changed =
        viewport_[0] != last_logged_viewport_[0] ||
        viewport_[1] != last_logged_viewport_[1] ||
        viewport_[2] != last_logged_viewport_[2] ||
        viewport_[3] != last_logged_viewport_[3];
    if (changed) {
        last_logged_viewport_[0] = viewport_[0];
        last_logged_viewport_[1] = viewport_[1];
        last_logged_viewport_[2] = viewport_[2];
        last_logged_viewport_[3] = viewport_[3];
        spdlog::debug(
            "[WindowsCompositorDebug] dcomp viewport rect normalized="
            "({:.5f},{:.5f})-({:.5f},{:.5f})",
            viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
    }
    work_pending_ = true;
    wake_.notify_one();
}

void WindowsNativeCompositor::SetViewportBackgroundColor(uint32_t argb) {
    std::lock_guard<std::mutex> lock(mutex_);
    viewport_background_[0] =
        static_cast<float>((argb >> 16) & 0xffu) / 255.0f;
    viewport_background_[1] =
        static_cast<float>((argb >> 8) & 0xffu) / 255.0f;
    viewport_background_[2] =
        static_cast<float>(argb & 0xffu) / 255.0f;
    viewport_background_[3] =
        static_cast<float>((argb >> 24) & 0xffu) / 255.0f;
    work_pending_ = true;
    wake_.notify_one();
}

bool WindowsNativeCompositor::RequestFlutterFrame(const std::string& reason) {
    Phase phase;
    uint64_t request_sequence = 0;
    uint64_t base_generation = 0;
    bool track_surface_update = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase = phase_;
        if (terminal_inactive_ || phase == Phase::Failed ||
            phase == Phase::Inactive) {
            return false;
        }
        request_sequence = ++flutter_frame_request_sequence_;
        base_generation = pending_flutter_frame_request_base_generation_;
        track_surface_update = phase != Phase::Preparing;
        if (track_surface_update) {
            pending_flutter_frame_request_sequence_ = request_sequence;
            pending_flutter_frame_request_base_generation_ =
                diagnostics_.flutter_generation;
            pending_flutter_frame_request_reason_ =
                reason.empty() ? "unspecified" : reason;
            pending_flutter_frame_request_time_ =
                std::chrono::steady_clock::now();
            last_explicit_flutter_frame_request_time_ =
                pending_flutter_frame_request_time_;
            pending_flutter_frame_request_acquire_logged_ = false;
            base_generation = pending_flutter_frame_request_base_generation_;
        } else {
            base_generation = diagnostics_.flutter_generation;
        }
    }
    const bool preparing = phase == Phase::Preparing;
    const bool ok = preparing
        ? (engine_api_.set_mode &&
           engine_api_.set_mode(flutter_view_, kExportMirror))
        : (engine_api_.request_frame &&
           engine_api_.request_frame(flutter_view_));
    spdlog::debug(
        "[WindowsCompositorDebug] request flutter frame reason={} "
        "phase={} action={} seq={} baseGeneration={} ok={}",
        reason.empty() ? "unspecified" : reason,
        PhaseName(phase),
        preparing ? "mirror-bootstrap" : "request-compositor-owned-export",
        request_sequence,
        base_generation,
        ok);
    if (ok && engine_api_.get_state && flutter_view_) {
        FlutterSurfaceExportState export_state = {};
        export_state.struct_size = sizeof(export_state);
        if (engine_api_.get_state(flutter_view_, &export_state)) {
            spdlog::debug(
                "[WindowsCompositorDebug] flutter export state after request "
                "seq={} requestCount={} dispatchCount={} publishCount={} "
                "scheduleCount={} vsyncCount={} presentCount={} pendingPump={} "
                "beginCount={} beginFail={} makeCurrentFail={} "
                "publishFail={} backpressure={} stateGeneration={} "
                "ringGeneration={} latestAvailable={}",
                request_sequence,
                export_state.request_count,
                export_state.request_dispatch_count,
                export_state.publish_count,
                export_state.schedule_frame_count,
                export_state.vsync_count,
                export_state.present_count,
                export_state.pending_frame_pump_frames,
                export_state.export_begin_count,
                export_state.export_begin_fail_count,
                export_state.export_make_current_fail_count,
                export_state.export_publish_fail_count,
                export_state.backpressure_count,
                export_state.frame_generation,
                export_state.ring_generation,
                export_state.latest_available);
        }
    }
    if (ok) SignalWork();
    return ok;
}

void WindowsNativeCompositor::BoostFlutterInteraction(
    const std::string& reason) {
    (void)reason;
    std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_inactive_ || phase_ == Phase::Failed ||
        phase_ == Phase::Inactive) {
        return;
    }
    last_explicit_flutter_frame_request_time_ =
        std::chrono::steady_clock::now();
    retained_flutter_content_dirty_ = true;
    work_pending_ = true;
    wake_.notify_one();
}

void WindowsNativeCompositor::SetSourceProjection(
    const SourceProjection& projection) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    source_projection_ = projection;
    diagnostics_.source_projection_enabled = projection.enabled;
    ++diagnostics_.source_projection_update_count;
    retained_projection_dirty_ = projection.enabled;
    if (projection.enabled) {
        last_retained_projection_update_ = now;
    }
    if (last_source_projection_debug_log_.time_since_epoch().count() == 0 ||
        now - last_source_projection_debug_log_ >=
            kSourceProjectionDebugSampleInterval) {
        last_source_projection_debug_log_ = now;
        spdlog::debug(
            "[WindowsCompositorDebug] source projection sample updates={} "
            "enabled={} mode={} split={} activeTracks={} retainedDirty={} "
            "heldSource={} retainedGraph={} deferredContent={} "
            "flutterDirty={} commits={} projectionCommits={}",
            diagnostics_.source_projection_update_count, projection.enabled,
            projection.mode, projection.split_pos,
            projection.active_track_count, retained_projection_dirty_,
            held_source_valid_, retained_graph_active_,
            retained_deferred_content_deadline_.time_since_epoch().count() != 0,
            retained_flutter_content_dirty_, retained_graph_commit_count_,
            retained_graph_projection_commit_count_);
    }
    work_pending_ = true;
    wake_.notify_one();
}

void WindowsNativeCompositor::ClearSourceProjection(
    const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_projection_ = {};
    diagnostics_.source_projection_enabled = false;
    diagnostics_.source_cache_active = false;
    source_cache_error_ = reason.empty() ? "clear-requested" : reason;
    diagnostics_.source_cache_last_error = source_cache_error_;
    retained_projection_dirty_ = false;
    last_retained_projection_update_ = {};
    retained_source_content_dirty_ = true;
    work_pending_ = true;
    wake_.notify_one();
}

void WindowsNativeCompositor::SetSourceCacheError(
    const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_cache_error_ = error.empty() ? "unknown" : error;
    diagnostics_.source_cache_last_error = source_cache_error_;
    ++diagnostics_.source_cache_fallback_count;
    work_pending_ = true;
    wake_.notify_one();
}

void WindowsNativeCompositor::NotifySourceCachePublished() {
    bool first_publish = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++source_cache_publish_count_;
        first_publish = source_cache_publish_count_ == 1;
        retained_source_content_dirty_ = true;
        work_pending_ = true;
    }
    if (first_publish) {
        spdlog::info(
            "[WindowsNativeCompositor] first source-cache publish notified");
    }
    wake_.notify_one();
}

void WindowsNativeCompositor::RequestOutputTarget(
    OutputTarget target,
    IDXGIAdapter* output_adapter,
    double sdr_white_level_nits,
    uint64_t display_generation,
    const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t next_output_high = output_luid_high_;
    uint32_t next_output_low = output_luid_low_;
    if (output_adapter) {
        (void)adapter_luid(output_adapter, next_output_high, next_output_low);
    }
    const double next_scale =
        std::isfinite(sdr_white_level_nits) &&
                sdr_white_level_nits > 0.0
            ? sdr_white_level_nits / 80.0
            : 1.0;
    const bool target_changed = target != desired_output_target_;
    const bool adapter_changed =
        !vr::windows_luid_equal(
            next_output_high, next_output_low,
            output_luid_high_, output_luid_low_);
    const bool white_changed =
        std::abs(
            next_scale -
            sdr_white_scale_.load(std::memory_order_relaxed)) > 0.0001;
    if (!target_changed && !white_changed && !adapter_changed &&
        display_generation == locked_display_generation_) {
        return;
    }
    if (adapter_changed && output_adapter) {
        pending_output_adapter_ = output_adapter;
        pending_output_luid_high_ = next_output_high;
        pending_output_luid_low_ = next_output_low;
        diagnostics_.pending_output_adapter_luid =
            luid_string(pending_output_luid_high_, pending_output_luid_low_);
    }
    desired_output_target_ = target;
    sdr_white_scale_ = next_scale;
    locked_display_generation_ = display_generation;
    diagnostics_.desired_output_target = OutputTargetName(target);
    diagnostics_.transition_state = "preparing";
    diagnostics_.transition_reason =
        reason.empty() ? "policy-refresh" : reason;
    diagnostics_.transition_serial++;
    transition_min_video_generation_ =
        target == OutputTarget::ScRGB ? diagnostics_.video_generation + 1 : 0;
    transition_min_source_generation_ =
        source_projection_.enabled
            ? diagnostics_.source_cache_consumed_generation + 1
            : 0;
    work_pending_ = true;
    wake_.notify_one();
}

void WindowsNativeCompositor::AcknowledgeFlutterState(
    uint64_t serial, bool transparent_viewport) {
    bool activate = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (serial != state_serial_) {
            spdlog::warn(
                "[WindowsNativeCompositor] ignoring stale Flutter ACK "
                "serial={} current={} transparent={}",
                serial, state_serial_, transparent_viewport);
            return;
        }
        ack_serial_ = serial;
        diagnostics_.ack_serial = serial;
        if (transparent_viewport && phase_ == Phase::Preparing) {
            activate = diagnostics_.flutter_generation > 0;
        }
        spdlog::info(
            "[WindowsNativeCompositor] Flutter ACK serial={} phase={} "
            "transparent={} generation={}",
            serial, PhaseName(phase_), transparent_viewport,
            diagnostics_.flutter_generation);
        work_pending_ = true;
    }
    wake_.notify_one();
    if (activate) {
        if (!engine_api_.set_mode ||
            !engine_api_.set_mode(flutter_view_, kExportCompositorOwned)) {
            EnterFailed("flutter-export-compositor-owned-failed");
            return;
        }
        PublishState(Phase::Active, "transparent-flutter-frame-acknowledged");
    }
}

void WindowsNativeCompositor::ForceFailureForTesting(
    const std::string& reason) {
    EnterFailed(reason.empty() ? "ui-test-forced-failure" : reason);
}

bool WindowsNativeCompositor::BeginDeviceRecovery(
    const std::string& reason,
    long removed_reason) {
    auto player = player_.lock();
    if (!player) {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.device_recovery_state =
            vr::windows_device_recovery_state_name(
                vr::WindowsDeviceRecoveryState::FailedTerminal);
        diagnostics_.device_recovery_preserved_player = false;
        ++diagnostics_.device_recovery_failure_count;
        diagnostics_.device_recovery_last_reason =
            reason.empty() ? "device-loss" : reason;
        diagnostics_.device_recovery_last_removed_reason =
            vr::windows_hresult_hex(static_cast<HRESULT>(removed_reason));
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (terminal_inactive_) {
        diagnostics_.device_recovery_state =
            vr::windows_device_recovery_state_name(
                vr::WindowsDeviceRecoveryState::FailedTerminal);
        ++diagnostics_.device_recovery_failure_count;
        diagnostics_.device_recovery_fallback_stage = "terminal-inactive";
        return false;
    }
    ++diagnostics_.device_recovery_generation;
    ++diagnostics_.device_recovery_attempt_count;
    diagnostics_.device_recovery_state =
        vr::windows_device_recovery_state_name(
            vr::WindowsDeviceRecoveryState::RebuildingPresentation);
    diagnostics_.device_recovery_last_reason =
        reason.empty() ? "device-loss" : reason;
    diagnostics_.device_recovery_last_removed_reason =
        vr::windows_hresult_hex(static_cast<HRESULT>(removed_reason));
    diagnostics_.device_recovery_fallback_stage = "none";
    diagnostics_.device_recovery_preserved_player = true;
    diagnostics_.device_recovery_last_frame_held =
        diagnostics_.present_count > 0;
    diagnostics_.transition_state = "rebuilding-presentation";
    diagnostics_.transition_reason =
        reason.empty() ? "device-loss" : reason;
    transition_min_video_generation_ =
        desired_output_target_ == OutputTarget::ScRGB
            ? diagnostics_.video_generation + 1
            : 0;
    transition_min_source_generation_ =
        source_projection_.enabled
            ? diagnostics_.source_cache_consumed_generation + 1
            : 0;
    pending_output_adapter_ = output_adapter_ ? output_adapter_ : producer_adapter_;
    work_pending_ = true;
    diagnostic_capture_pending_ = true;
    spdlog::warn(
        "[WindowsDeviceRecovery] compositor rebuild scheduled reason={} removed=0x{:08x}",
        diagnostics_.device_recovery_last_reason,
        static_cast<uint32_t>(removed_reason));
    wake_.notify_one();
    return true;
}

WindowsNativeCompositor::Diagnostics
WindowsNativeCompositor::diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Diagnostics result = diagnostics_;
    const bool compositor_accepts_source_cache =
        phase_ == Phase::Preparing || phase_ == Phase::Active;
    if (!compositor_accepts_source_cache) {
        result.source_projection_enabled = false;
        result.source_cache_active = false;
    }
    const auto high_refresh = high_refresh_metrics_.snapshot();
    result.high_refresh_gate_supported = high_refresh.gate_supported;
    result.high_refresh_display_hz = high_refresh.display_hz;
    result.dcomp_present_interval_p95_us =
        high_refresh.present_interval_p95_us;
    result.dcomp_composite_p95_us = high_refresh.composite_p95_us;
    result.dcomp_draw_p95_us = high_refresh.draw_p95_us;
    result.dcomp_present_block_p95_us = high_refresh.present_block_p95_us;
    result.dcomp_acquire_wait_p95_us = high_refresh.acquire_wait_p95_us;
    result.interaction_input_to_present_p95_us =
        high_refresh.interaction_input_to_present_p95_us;
    result.dcomp_drop_rate_x1000 = high_refresh.drop_rate_x1000;
    result.source_projection_reuse_count =
        high_refresh.source_projection_reuse_count;
    result.viewport_redraw_during_projection_count =
        high_refresh.viewport_redraw_during_projection_count;
    result.overlay_layer_raster_count =
        high_refresh.overlay_layer_raster_count;
    result.overlay_layer_upload_count =
        high_refresh.overlay_layer_upload_count;
    result.overlay_layer_reuse_count =
        high_refresh.overlay_layer_reuse_count;
    result.overlay_composite_p95_us = high_refresh.overlay_composite_p95_us;
    result.overlay_raster_p95_us = high_refresh.overlay_raster_p95_us;
    result.overlay_upload_p95_us = high_refresh.overlay_upload_p95_us;
    const auto overlay_state = overlay_layer_state_.snapshot();
    result.overlay_retained_layer_active = overlay_state.active;
    result.overlay_layer_mode = overlay_state.mode;
    result.overlay_layer_texture_count = overlay_state.texture_count;
    result.overlay_layer_bytes = overlay_state.bytes;
    result.overlay_layer_generation = overlay_state.generation;
    result.overlay_layer_committed_generation =
        overlay_state.committed_generation;
    result.overlay_layer_pending_generation =
        overlay_state.pending_generation;
    result.overlay_layer_composite_count = overlay_state.composite_count;
    result.overlay_layer_miss_count = overlay_state.miss_count;
    result.overlay_layer_backpressure_count =
        overlay_state.backpressure_count;
    result.overlay_layer_fallback_reason = overlay_state.fallback_reason;
    result.overlay_layer_last_error = overlay_state.last_error;
    result.flutter_export_unsolicited_signal_count =
        flutter_export_unsolicited_signal_count_;
    result.flutter_export_unsolicited_throttle_count =
        flutter_export_unsolicited_throttle_count_;
    const bool hot_path_active =
        phase_ == Phase::Active && result.source_projection_enabled;
    const auto gate_result = vr::evaluate_windows_high_refresh_gate(
        high_refresh, hot_path_active, overlay_state.active);
    result.high_refresh_gate_last_result = gate_result;
    result.hot_path_active = hot_path_active;
    result.hot_path_mode =
        hot_path_active ? "source-projection-dcomp" : "inactive";
    result.hot_path_display_hz = high_refresh.display_hz;
    result.hot_path_frame_budget_us =
        high_refresh.display_hz > 0 ? 1000000 / high_refresh.display_hz
                                    : 16666;
    result.hot_path_present_interval_p95_us =
        high_refresh.present_interval_p95_us;
    result.hot_path_composite_p95_us = high_refresh.composite_p95_us;
    result.hot_path_draw_p95_us = high_refresh.draw_p95_us;
    result.hot_path_present_block_p95_us = high_refresh.present_block_p95_us;
    result.hot_path_acquire_wait_p95_us = high_refresh.acquire_wait_p95_us;
    result.hot_path_input_to_present_p95_us =
        high_refresh.interaction_input_to_present_p95_us;
    result.hot_path_drop_rate_x1000 = high_refresh.drop_rate_x1000;
    result.hot_path_projection_only_update_count =
        result.source_projection_update_count;
    result.hot_path_viewport_redraw_during_projection_count =
        high_refresh.viewport_redraw_during_projection_count;
    result.hot_path_source_cache_reuse_count =
        high_refresh.source_projection_reuse_count;
    result.hot_path_overlay_reuse_count =
        high_refresh.overlay_layer_reuse_count;
    result.hot_path_overlay_raster_count =
        high_refresh.overlay_layer_raster_count;
    result.hot_path_overlay_upload_count =
        high_refresh.overlay_layer_upload_count;
    result.hot_path_gate_result = gate_result;
    result.hot_path_last_failure_reason =
        gate_result.rfind("fail-", 0) == 0 ? gate_result : "none";
    result.retained_graph_active = retained_graph_active_;
    result.retained_graph_mode =
        retained_graph_active_ ? "active" : "inactive";
    result.retained_graph_fallback_reason =
        retained_graph_fallback_reason_;
    result.retained_graph_commit_count =
        retained_graph_commit_count_;
    result.retained_graph_projection_commit_count =
        retained_graph_projection_commit_count_;
    result.retained_graph_source_bake_count =
        retained_graph_source_bake_count_;
    result.retained_graph_flutter_bake_count =
        retained_graph_flutter_bake_count_;
    result.retained_graph_projection_skip_present_count =
        retained_graph_projection_skip_present_count_;
    result.retained_graph_deferred_content_count =
        retained_graph_deferred_content_count_;
    result.retained_graph_commit_defer_count =
        retained_graph_commit_defer_count_;
    result.retained_graph_flutter_bake_p95_us =
        retained_graph_p95(retained_graph_flutter_bake_us_);
    result.retained_graph_source_bake_p95_us =
        retained_graph_p95(retained_graph_source_bake_us_);
    result.retained_graph_apply_p95_us =
        retained_graph_p95(retained_graph_apply_us_);
    result.retained_graph_commit_p95_us =
        retained_graph_p95(retained_graph_commit_us_);
    const double elapsed_seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - rate_start_time_).count();
    if (elapsed_seconds > 0.0) {
        result.source_cache_hz =
            static_cast<double>(source_cache_publish_count_) /
            elapsed_seconds;
        result.source_projection_hz =
            static_cast<double>(result.source_projection_update_count) /
            elapsed_seconds;
    }
    return result;
}

void WindowsNativeCompositor::SetHighRefreshDisplayHz(int64_t display_hz) {
    std::lock_guard<std::mutex> lock(mutex_);
    high_refresh_metrics_.set_display_hz(display_hz);
    diagnostics_.high_refresh_display_hz = display_hz > 0 ? display_hz : 60;
}

void WindowsNativeCompositor::ResetHighRefreshMetrics() {
    std::lock_guard<std::mutex> lock(mutex_);
    high_refresh_metrics_.reset(diagnostics_.high_refresh_display_hz);
    retained_graph_flutter_bake_us_.clear();
    retained_graph_source_bake_us_.clear();
    retained_graph_apply_us_.clear();
    retained_graph_commit_us_.clear();
    retained_graph_commit_deadline_ = {};
    last_retained_graph_commit_time_ = {};
    last_present_time_ = {};
    interaction_sample_started_ = {};
    last_overlay_metrics_generation_ = 0;
    interaction_sample_active_ = false;
}

void WindowsNativeCompositor::BeginInteractionSample(
    const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex_);
    interaction_sample_started_ = std::chrono::steady_clock::now();
    interaction_sample_active_ = true;
    spdlog::info(
        "[WindowsHighRefresh] begin interaction sample label={}",
        label.empty() ? "unnamed" : label);
}

void WindowsNativeCompositor::EndInteractionSample(
    const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex_);
    interaction_sample_active_ = false;
    spdlog::info(
        "[WindowsHighRefresh] end interaction sample label={}",
        label.empty() ? "unnamed" : label);
}

void WindowsNativeCompositor::RequestDiagnosticCapture() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostic_capture_pending_ = true;
        work_pending_ = true;
    }
    wake_.notify_one();
}

void WindowsNativeCompositor::OnFlutterSurfacePublished(
    void*, uint64_t generation, void* user_data) {
    auto* compositor = static_cast<WindowsNativeCompositor*>(user_data);
    if (!compositor) return;
    const auto now = std::chrono::steady_clock::now();
    uint64_t request_sequence = 0;
    uint64_t base_generation = 0;
    std::string reason;
    int64_t elapsed_ms = 0;
    bool log_publish = false;
    bool signal_work = false;
    {
        std::lock_guard<std::mutex> lock(compositor->mutex_);
        ++compositor->flutter_publish_callback_count_;
        compositor->last_flutter_publish_callback_generation_ = generation;
        request_sequence =
            compositor->pending_flutter_frame_request_sequence_;
        base_generation =
            compositor->pending_flutter_frame_request_base_generation_;
        reason = compositor->pending_flutter_frame_request_reason_;
        if (request_sequence > 0) {
            elapsed_ms = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() -
                    compositor->pending_flutter_frame_request_time_)
                    .count());
            log_publish = true;
            signal_work = true;
            compositor->retained_flutter_content_dirty_ = true;
        } else if (compositor->phase_ != Phase::Active) {
            signal_work = true;
            compositor->retained_flutter_content_dirty_ = true;
        } else {
            const bool flutter_content_already_dirty =
                compositor->retained_flutter_content_dirty_;
            const bool deferred_content_wake_pending =
                compositor->retained_deferred_content_deadline_
                    .time_since_epoch()
                    .count() != 0;
            compositor->retained_flutter_content_dirty_ = true;
            if (flutter_content_already_dirty ||
                deferred_content_wake_pending) {
                ++compositor->flutter_export_unsolicited_throttle_count_;
                return;
            }
            const int64_t display_hz = std::max<int64_t>(
                1, compositor->diagnostics_.high_refresh_display_hz);
            const int64_t signal_interval_us = std::clamp<int64_t>(
                1000000 / display_hz,
                kMinUnrequestedFlutterExportSignalUs,
                kMaxUnrequestedFlutterExportSignalUs);
            const auto signal_interval =
                std::chrono::microseconds(signal_interval_us);
            if (compositor->last_unsolicited_flutter_export_signal_
                       .time_since_epoch()
                       .count() == 0 ||
                    now - compositor->last_unsolicited_flutter_export_signal_ >=
                        signal_interval) {
                compositor->last_unsolicited_flutter_export_signal_ = now;
                ++compositor->flutter_export_unsolicited_signal_count_;
                signal_work = true;
            } else {
                ++compositor->flutter_export_unsolicited_throttle_count_;
            }
        }
    }
    if (log_publish) {
        spdlog::debug(
            "[WindowsCompositorDebug] flutter surface published after "
            "request seq={} reason={} baseGeneration={} "
            "publishedGeneration={} elapsedMs={}",
            request_sequence,
            reason.empty() ? "unspecified" : reason,
            base_generation,
            generation,
            elapsed_ms);
    }
    if (signal_work) {
        compositor->SignalWork();
    }
}

bool WindowsNativeCompositor::LoadEngineApi() {
    HMODULE module = GetModuleHandleW(L"flutter_windows.dll");
    if (!module) return false;
    engine_api_.set_mode = reinterpret_cast<SetExportModeFn>(
        GetProcAddress(module, "FlutterDesktopViewSetSurfaceExportMode"));
    engine_api_.request_frame = reinterpret_cast<RequestSurfaceExportFrameFn>(
        GetProcAddress(module, "FlutterDesktopViewRequestSurfaceExportFrame"));
    engine_api_.get_state = reinterpret_cast<GetSurfaceExportStateFn>(
        GetProcAddress(module, "FlutterDesktopViewGetSurfaceExportState"));
    engine_api_.set_callback = reinterpret_cast<SetPublishedCallbackFn>(
        GetProcAddress(module, "FlutterDesktopViewSetSurfacePublishedCallback"));
    engine_api_.acquire = reinterpret_cast<AcquireFlutterSurfaceFn>(
        GetProcAddress(module, "FlutterDesktopViewAcquireLatestSurface"));
    engine_api_.release = reinterpret_cast<ReleaseFlutterSurfaceFn>(
        GetProcAddress(module, "FlutterDesktopViewReleaseSurface"));
    return engine_api_.available();
}

bool WindowsNativeCompositor::EnsureProducerDevice(IDXGIAdapter* producer_adapter) {
    if (!producer_adapter) {
        return false;
    }
    producer_adapter_ = producer_adapter;
    (void)adapter_luid(
        producer_adapter_.Get(), producer_luid_high_, producer_luid_low_);
    if (producer_device_ && producer_context_) {
        return true;
    }
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = D3D11CreateDevice(
        producer_adapter_.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &producer_device_,
        &level,
        &producer_context_);
    if (FAILED(hr)) {
        spdlog::error(
            "[WindowsNativeCompositor] producer D3D11CreateDevice failed hr=0x{:08x}",
            static_cast<uint32_t>(hr));
        producer_device_.Reset();
        producer_context_.Reset();
        return false;
    }
    return true;
}

bool WindowsNativeCompositor::IsCrossAdapterActive() const {
    return vr::windows_cross_adapter_required(
        producer_luid_high_, producer_luid_low_,
        output_luid_high_, output_luid_low_);
}

bool WindowsNativeCompositor::InitializeDeviceAndComposition(
    IDXGIAdapter* producer_adapter,
    IDXGIAdapter* output_adapter) {
    const auto log_failure = [](const char* stage, HRESULT result) {
        spdlog::error(
            "[WindowsNativeCompositor] {} failed hr=0x{:08x}",
            stage,
            static_cast<uint32_t>(result));
        return false;
    };
    if (!EnsureProducerDevice(producer_adapter)) {
        return false;
    }
    output_adapter_ = output_adapter ? output_adapter : producer_adapter;
    (void)adapter_luid(
        output_adapter_.Get(), output_luid_high_, output_luid_low_);

    pending_swap_chain_ = {};
    current_swap_chain_ = {};
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();
    ResetRetainedGraph("device-rebuild");
    constants_.Reset();
    sampler_.Reset();
    pixel_shader_.Reset();
    video_pixel_shader_.Reset();
    flutter_pixel_shader_.Reset();
    premultiplied_blend_state_.Reset();
    overlay_blend_state_.Reset();
    overlay_input_layout_.Reset();
    overlay_pixel_shader_.Reset();
    overlay_vertex_shader_.Reset();
    ResetOverlayLayer("device-rebuild");
    vertex_shader_.Reset();
    context_.Reset();
    device_.Reset();
    video_transport_.reset();
    sdr_video_transport_.reset();
    flutter_transport_.reset();
    for (auto& transport : source_transports_) {
        transport.reset();
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = D3D11CreateDevice(
        output_adapter_.Get(),
        output_adapter_ ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
        &device_, &level, &context_);
    if (FAILED(hr)) return log_failure("D3D11CreateDevice", hr);
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    hr = device_.As(&dxgi_device);
    if (FAILED(hr)) return log_failure("Query IDXGIDevice", hr);
    hr = DCompositionCreateDevice(
        dxgi_device.Get(), IID_PPV_ARGS(&dcomp_device_));
    if (FAILED(hr)) return log_failure("DCompositionCreateDevice", hr);
    hr = dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_);
    if (FAILED(hr)) return log_failure("CreateTargetForHwnd", hr);
    hr = dcomp_device_->CreateVisual(&dcomp_visual_);
    if (FAILED(hr)) {
        return log_failure("CreateVisual", hr);
    }
    transport_support_ = vr::probe_windows_cross_adapter_transport(
        producer_device_.Get(), device_.Get());
    if (IsCrossAdapterActive() && !transport_support_.bgra8) {
        spdlog::warn(
            "[WindowsNativeCompositor] cross-adapter BGRA transport unavailable; "
            "staying on producer adapter status={}",
            transport_support_.status);
        return InitializeDeviceAndComposition(
            producer_adapter_.Get(), producer_adapter_.Get());
    }
    return true;
}

bool WindowsNativeCompositor::CreateSwapChainCandidate(
    uint32_t width,
    uint32_t height,
    OutputTarget target,
    SwapChainResources& resources) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.As(&dxgi_device)) ||
        FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = target == OutputTarget::ScRGB
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
    HRESULT hr = factory->CreateSwapChainForComposition(
        device_.Get(), &desc, nullptr, &chain);
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;
    if (FAILED(hr) || FAILED(chain.As(&swap_chain))) return false;
    const DXGI_COLOR_SPACE_TYPE color_space =
        target == OutputTarget::ScRGB
            ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT color_support = 0;
    if (FAILED(swap_chain->CheckColorSpaceSupport(
            color_space, &color_support)) ||
        (color_support &
         DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0 ||
        FAILED(swap_chain->SetColorSpace1(color_space))) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
        FAILED(device_->CreateRenderTargetView(
            back_buffer.Get(), nullptr, &rtv))) {
        return false;
    }
    resources = {};
    resources.swap_chain = std::move(swap_chain);
    resources.rtv = std::move(rtv);
    resources.target = target;
    resources.width = width;
    resources.height = height;
    resources.color_space_supported = true;
    return true;
}

bool WindowsNativeCompositor::OpenInputTexture(
    ID3D11Device1* device1,
    HANDLE handle,
    ID3D11Texture2D** texture) const {
    if (!handle || !texture) {
        return false;
    }
    *texture = nullptr;
    if (IsCrossAdapterActive()) {
        Microsoft::WRL::ComPtr<ID3D11Device1> producer1;
        if (FAILED(producer_device_.As(&producer1)) || !producer1) {
            return false;
        }
        return SUCCEEDED(
            producer1->OpenSharedResource1(handle, IID_PPV_ARGS(texture)));
    }
    return device1 &&
           SUCCEEDED(device1->OpenSharedResource1(
               handle, IID_PPV_ARGS(texture)));
}

bool WindowsNativeCompositor::TransportInput(
    ID3D11Texture2D* producer_texture,
    DXGI_FORMAT format,
    uint32_t width,
    uint32_t height,
    vr::D3D11CrossAdapterTextureTransport& transport,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv) {
    if (!IsCrossAdapterActive()) {
        return false;
    }
    if (!producer_texture || !producer_device_ || !producer_context_ ||
        !device_ || !context_) {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.cross_adapter_last_error = "transport-missing-device";
        return false;
    }
    if (transport.format() != format ||
        transport.width() != width ||
        transport.height() != height) {
        if (!transport.initialize(
                producer_device_.Get(),
                producer_context_.Get(),
                device_.Get(),
                context_.Get(),
                format,
                width,
                height,
                cross_adapter_sync_request_)) {
            std::lock_guard<std::mutex> lock(mutex_);
            diagnostics_.cross_adapter_last_error = transport.last_error();
            diagnostics_.cross_adapter_sync_fallback_reason =
                transport.sync_fallback_reason();
            ++diagnostics_.output_migration_failure_count;
            return false;
        }
    }
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> next_srv;
    ID3D11ShaderResourceView* raw_srv = nullptr;
    if (!transport.copy_to_output_srv(producer_texture, &raw_srv) ||
        !raw_srv) {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.cross_adapter_last_error = transport.last_error();
        ++diagnostics_.transport_timeout_count;
        return false;
    }
    next_srv.Attach(raw_srv);
    srv = std::move(next_srv);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.cross_adapter_last_error = "none";
        ++diagnostics_.transport_generation;
        diagnostics_.transport_copy_count =
            video_transport_.copy_count() +
            sdr_video_transport_.copy_count() +
            flutter_transport_.copy_count();
        diagnostics_.transport_copy_bytes =
            video_transport_.copy_count() * video_transport_.bytes_per_copy() +
            sdr_video_transport_.copy_count() *
                sdr_video_transport_.bytes_per_copy() +
            flutter_transport_.copy_count() *
                flutter_transport_.bytes_per_copy();
        diagnostics_.transport_timeout_count =
            video_transport_.timeout_count() +
            sdr_video_transport_.timeout_count() +
            flutter_transport_.timeout_count();
        diagnostics_.transport_last_copy_us = transport.last_copy_us();
        diagnostics_.transport_total_copy_us =
            video_transport_.total_copy_us() +
            sdr_video_transport_.total_copy_us() +
            flutter_transport_.total_copy_us();
        diagnostics_.shared_fence_signal_count =
            video_transport_.shared_fence_signal_count() +
            sdr_video_transport_.shared_fence_signal_count() +
            flutter_transport_.shared_fence_signal_count();
        diagnostics_.shared_fence_wait_count =
            video_transport_.shared_fence_wait_count() +
            sdr_video_transport_.shared_fence_wait_count() +
            flutter_transport_.shared_fence_wait_count();
        diagnostics_.shared_fence_timeout_count =
            video_transport_.shared_fence_timeout_count() +
            sdr_video_transport_.shared_fence_timeout_count() +
            flutter_transport_.shared_fence_timeout_count();
        diagnostics_.shared_fence_last_wait_us =
            transport.shared_fence_last_wait_us();
        diagnostics_.shared_fence_p95_wait_us =
            std::max({
                video_transport_.shared_fence_p95_wait_us(),
                sdr_video_transport_.shared_fence_p95_wait_us(),
                flutter_transport_.shared_fence_p95_wait_us()});
        diagnostics_.event_query_p95_wait_us =
            std::max({
                video_transport_.event_query_p95_wait_us(),
                sdr_video_transport_.event_query_p95_wait_us(),
                flutter_transport_.event_query_p95_wait_us()});
        diagnostics_.cross_adapter_requested_sync_kind =
            transport.requested_sync_kind();
        diagnostics_.cross_adapter_active_sync_kind =
            transport.active_sync_kind();
        diagnostics_.cross_adapter_sync_kind = transport.active_sync_kind();
        diagnostics_.cross_adapter_sync_fallback_reason =
            transport.sync_fallback_reason();
        diagnostics_.transport_shared_fence_handle_created =
            diagnostics_.transport_shared_fence_handle_created ||
            transport.shared_fence_handle_created();
        diagnostics_.transport_shared_fence_open_succeeded =
            diagnostics_.transport_shared_fence_open_succeeded ||
            transport.shared_fence_open_succeeded();
        for (const auto& source_transport : source_transports_) {
            diagnostics_.transport_copy_count +=
                source_transport.copy_count();
            diagnostics_.transport_copy_bytes +=
                source_transport.copy_count() *
                source_transport.bytes_per_copy();
            diagnostics_.transport_timeout_count +=
                source_transport.timeout_count();
            diagnostics_.transport_total_copy_us +=
                source_transport.total_copy_us();
            diagnostics_.shared_fence_signal_count +=
                source_transport.shared_fence_signal_count();
            diagnostics_.shared_fence_wait_count +=
                source_transport.shared_fence_wait_count();
            diagnostics_.shared_fence_timeout_count +=
                source_transport.shared_fence_timeout_count();
            diagnostics_.shared_fence_p95_wait_us = std::max(
                diagnostics_.shared_fence_p95_wait_us,
                source_transport.shared_fence_p95_wait_us());
            diagnostics_.event_query_p95_wait_us = std::max(
                diagnostics_.event_query_p95_wait_us,
                source_transport.event_query_p95_wait_us());
        }
    }
    return true;
}

bool WindowsNativeCompositor::EnsureSwapChain(
    uint32_t width, uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    OutputTarget desired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        desired = desired_output_target_;
        if (desired == OutputTarget::ScRGB &&
            IsCrossAdapterActive() &&
            !transport_support_.rgba16f) {
            desired = OutputTarget::SDR;
            desired_output_target_ = OutputTarget::SDR;
            diagnostics_.desired_output_target = OutputTargetName(desired);
            diagnostics_.transition_reason =
                "cross-adapter-fp16-transport-unavailable-fallback-sdr";
            ++diagnostics_.target_fallback_count;
        }
    }
    const auto matches = [&](const SwapChainResources& resources) {
        return resources.swap_chain &&
               resources.width == width &&
               resources.height == height &&
               resources.target == desired;
    };
    if (pending_swap_chain_.swap_chain &&
        !matches(pending_swap_chain_)) {
        pending_swap_chain_ = {};
    }
    if (matches(pending_swap_chain_) || matches(current_swap_chain_)) {
        return true;
    }
    SwapChainResources candidate;
    if (CreateSwapChainCandidate(width, height, desired, candidate)) {
        pending_swap_chain_ = std::move(candidate);
        return true;
    }
    if (desired != OutputTarget::ScRGB ||
        !CreateSwapChainCandidate(
            width, height, OutputTarget::SDR, candidate)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++diagnostics_.target_fallback_count;
        diagnostics_.transition_reason =
            "hdr-target-unavailable-fallback-sdr";
        desired_output_target_ = OutputTarget::SDR;
    }
    pending_swap_chain_ = std::move(candidate);
    return true;
}

bool WindowsNativeCompositor::ActivatePendingSwapChain() {
    if (!pending_swap_chain_.swap_chain) {
        return true;
    }
    if (FAILED(dcomp_visual_->SetContent(
            pending_swap_chain_.swap_chain.Get())) ||
        FAILED(dcomp_target_->SetRoot(dcomp_visual_.Get())) ||
        FAILED(dcomp_device_->Commit())) {
        return false;
    }
    const bool had_output =
        static_cast<bool>(current_swap_chain_.swap_chain);
    const OutputTarget previous = current_swap_chain_.target;
    const uint32_t previous_width = current_swap_chain_.width;
    const uint32_t previous_height = current_swap_chain_.height;
    current_swap_chain_ = std::move(pending_swap_chain_);
    pending_swap_chain_ = {};
    spdlog::info(
        "[WindowsCompositorDebug] dcomp activate swapchain target={} "
        "size={}x{} previous={}x{} hadOutput={}",
        OutputTargetName(current_swap_chain_.target),
        current_swap_chain_.width,
        current_swap_chain_.height,
        previous_width,
        previous_height,
        had_output);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.swap_chain_active = true;
        diagnostics_.swap_chain_width = current_swap_chain_.width;
        diagnostics_.swap_chain_height = current_swap_chain_.height;
        diagnostics_.output_target =
            OutputTargetName(current_swap_chain_.target);
        diagnostics_.swap_chain_format =
            OutputFormatName(current_swap_chain_.target);
        diagnostics_.color_space =
            OutputColorSpaceName(current_swap_chain_.target);
        diagnostics_.color_space_supported =
            current_swap_chain_.color_space_supported;
        diagnostics_.sdr_tone_map_active =
            current_swap_chain_.target == OutputTarget::SDR;
        diagnostics_.transition_state = "stable";
        ++diagnostics_.output_generation;
        if (had_output &&
            (previous_width != current_swap_chain_.width ||
             previous_height != current_swap_chain_.height)) {
            ++diagnostics_.resize_count;
        }
        if (had_output && previous != current_swap_chain_.target) {
            if (current_swap_chain_.target == OutputTarget::ScRGB) {
                ++diagnostics_.hdr_promotion_count;
            } else {
                ++diagnostics_.hdr_demotion_count;
            }
        }
    }
    return true;
}

bool WindowsNativeCompositor::EnsureRetainedGraph(
    uint32_t width, uint32_t height, OutputTarget target) {
    if (!dcomp_device_) {
        retained_graph_fallback_reason_ = "dcomp-device-unavailable";
        return false;
    }
    if (retained_graph_target_ != target &&
        (retained_root_visual_ || retained_background_.surface ||
         retained_flutter_.surface)) {
        ResetRetainedGraph("target-changed");
    }
    retained_graph_target_ = target;
    const DXGI_FORMAT surface_format = retained_surface_format(target);
    const auto log_failure = [&](const char* stage, HRESULT result) {
        retained_graph_fallback_reason_ = stage;
        spdlog::warn(
            "[WindowsNativeCompositor] retained graph {} failed hr=0x{:08x}",
            stage,
            static_cast<uint32_t>(result));
        return false;
    };
    HRESULT hr = S_OK;
    if (!retained_root_visual_) {
        hr = dcomp_device_->CreateVisual(&retained_root_visual_);
        if (FAILED(hr)) return log_failure("create-root-visual", hr);
    }
    if (!retained_background_visual_) {
        hr = dcomp_device_->CreateVisual(&retained_background_visual_);
        if (FAILED(hr)) return log_failure("create-background-visual", hr);
        hr = retained_root_visual_->AddVisual(
            retained_background_visual_.Get(), FALSE, nullptr);
        if (FAILED(hr)) return log_failure("add-background-visual", hr);
    }
    if (!retained_source_root_visual_) {
        hr = dcomp_device_->CreateVisual(&retained_source_root_visual_);
        if (FAILED(hr)) return log_failure("create-source-root-visual", hr);
        hr = retained_root_visual_->AddVisual(
            retained_source_root_visual_.Get(),
            TRUE,
            retained_background_visual_.Get());
        if (FAILED(hr)) return log_failure("add-source-root-visual", hr);
    }
    if (!retained_flutter_.visual) {
        hr = dcomp_device_->CreateVisual(&retained_flutter_.visual);
        if (FAILED(hr)) return log_failure("create-flutter-visual", hr);
        hr = retained_root_visual_->AddVisual(
            retained_flutter_.visual.Get(),
            TRUE,
            retained_source_root_visual_.Get());
        if (FAILED(hr)) return log_failure("add-flutter-visual", hr);
    }
    if (!retained_background_.surface) {
        hr = dcomp_device_->CreateSurface(
            1, 1, surface_format,
            DXGI_ALPHA_MODE_IGNORE, &retained_background_.surface);
        if (FAILED(hr)) return log_failure("create-background-surface", hr);
        retained_background_.format = surface_format;
        retained_background_.width = 1;
        retained_background_.height = 1;
        retained_background_.ready = true;
        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        POINT offset = {};
        RECT rect = {0, 0, 1, 1};
        hr = retained_background_.surface->BeginDraw(
            &rect, IID_PPV_ARGS(&surface), &offset);
        if (FAILED(hr)) return log_failure("draw-background-begin", hr);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(surface.As(&texture)) &&
            SUCCEEDED(device_->CreateRenderTargetView(
                texture.Get(), nullptr, &rtv))) {
            const float black[4] = {
                viewport_background_[0],
                viewport_background_[1],
                viewport_background_[2],
                1.0f};
            context_->ClearRenderTargetView(rtv.Get(), black);
            context_->Flush();
        }
        retained_background_.surface->EndDraw();
        hr = retained_background_visual_->SetContent(
            retained_background_.surface.Get());
        if (FAILED(hr)) return log_failure("set-background-content", hr);
    }
    retained_background_visual_->SetOffsetX(0.0f);
    retained_background_visual_->SetOffsetY(0.0f);
    const D2D_MATRIX_3X2_F background_transform = {
        static_cast<float>(std::max(width, 1u)), 0.0f,
        0.0f, static_cast<float>(std::max(height, 1u)),
        0.0f, 0.0f};
    retained_background_visual_->SetTransform(background_transform);
    retained_graph_fallback_reason_ = "none";
    return true;
}

bool WindowsNativeCompositor::BakeRetainedSourceSurface(
    size_t slot,
    ID3D11ShaderResourceView* source_srv,
    uint32_t width,
    uint32_t height,
    OutputTarget target,
    int color_transfer,
    uint64_t generation,
    const std::shared_ptr<const vr::AnalysisOverlayPrimitivePackage>& overlay) {
    if (slot >= retained_sources_.size() || !source_srv ||
        width == 0 || height == 0 ||
        !EnsureRetainedGraph(width, height, target)) {
        retained_graph_fallback_reason_ = "invalid-source-bake";
        return false;
    }
    auto& layer = retained_sources_[slot];
    const DXGI_FORMAT surface_format = retained_surface_format(target);
    const auto log_failure = [&](const char* stage, HRESULT result) {
        retained_graph_fallback_reason_ = stage;
        spdlog::warn(
            "[WindowsNativeCompositor] retained source {} {} failed hr=0x{:08x}",
            slot,
            stage,
            static_cast<uint32_t>(result));
        return false;
    };
    HRESULT hr = S_OK;
    if (!layer.surface || layer.width != width || layer.height != height ||
        layer.format != surface_format) {
        layer = {};
        hr = dcomp_device_->CreateSurface(
            width, height, surface_format,
            DXGI_ALPHA_MODE_IGNORE, &layer.surface);
        if (FAILED(hr)) return log_failure("create-surface", hr);
        hr = dcomp_device_->CreateVisual(&layer.visual);
        if (FAILED(hr)) return log_failure("create-visual", hr);
        hr = dcomp_device_->CreateRectangleClip(&layer.clip);
        if (FAILED(hr)) return log_failure("create-clip", hr);
        hr = layer.visual->SetContent(layer.surface.Get());
        if (FAILED(hr)) return log_failure("set-content", hr);
        hr = layer.visual->SetClip(layer.clip.Get());
        if (FAILED(hr)) return log_failure("set-clip", hr);
        hr = retained_source_root_visual_->AddVisual(
            layer.visual.Get(), TRUE, nullptr);
        if (FAILED(hr)) return log_failure("add-visual", hr);
        layer.width = width;
        layer.height = height;
        layer.format = surface_format;
    }

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    POINT offset = {};
    RECT rect = {
        0, 0,
        static_cast<LONG>(width),
        static_cast<LONG>(height)};
    hr = layer.surface->BeginDraw(&rect, IID_PPV_ARGS(&surface), &offset);
    if (FAILED(hr)) return log_failure("begin-draw", hr);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    bool ok = SUCCEEDED(surface.As(&texture)) &&
              SUCCEEDED(device_->CreateRenderTargetView(
                  texture.Get(), nullptr, &rtv));
    if (ok) {
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        ID3D11RenderTargetView* raw_rtv = rtv.Get();
        context_->OMSetRenderTargets(1, &raw_rtv, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        std::array<ID3D11ShaderResourceView*, 7> srvs = {};
        srvs[2 + slot] = source_srv;
        context_->PSSetShaderResources(
            0, static_cast<UINT>(srvs.size()), srvs.data());
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        CompositeConstants values = {};
        values.viewport[0] = 0.0f;
        values.viewport[1] = 0.0f;
        values.viewport[2] = 1.0f;
        values.viewport[3] = 1.0f;
        values.sdr_white_scale = static_cast<float>(
            sdr_white_scale_.load(std::memory_order_relaxed));
        values.output_mode = target == OutputTarget::ScRGB ? 1.0f : 0.0f;
        values.source_projection_enabled = 1.0f;
        values.source_mode = 0.0f;
        values.source_split_pos = 0.5f;
        values.source_track_count = 1.0f;
        values.source_present[slot] = 1.0f;
        values.source_order[0] = static_cast<float>(slot);
        values.source_transfer[slot] = static_cast<float>(color_transfer);
        values.source_inv_display_size_x[slot] = 1.0f;
        values.source_inv_display_size_y[slot] = 1.0f;
        values.background_color[3] = 1.0f;
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
        ID3D11Buffer* constants = constants_.Get();
        context_->PSSetConstantBuffers(0, 1, &constants);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context_->PSSetShader(video_pixel_shader_.Get(), nullptr, 0);
        context_->Draw(4, 0);
        if (overlay) {
            D3D11_TEXTURE2D_DESC retained_desc = {};
            retained_desc.Width = width;
            retained_desc.Height = height;
            retained_desc.Format = surface_format;
            (void)DrawOverlay(overlay, SourceProjection{}, retained_desc);
            context_->IASetInputLayout(nullptr);
            context_->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        }
        std::array<ID3D11ShaderResourceView*, 7> null_srvs = {};
        context_->PSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    }
    const HRESULT end_result = layer.surface->EndDraw();
    if (!ok) return log_failure("render-target-view", E_FAIL);
    if (FAILED(end_result)) return log_failure("end-draw", end_result);
    layer.generation = generation;
    layer.ready = true;
    ++retained_graph_source_bake_count_;
    retained_graph_fallback_reason_ = "none";
    return true;
}

bool WindowsNativeCompositor::BakeRetainedFlutterSurface(
    const FlutterSurface& surface_info,
    OutputTarget target,
    ID3D11ShaderResourceView* flutter_srv) {
    if (!flutter_srv || surface_info.width == 0 ||
        surface_info.height == 0 ||
        !EnsureRetainedGraph(surface_info.width, surface_info.height, target)) {
        retained_graph_fallback_reason_ = "invalid-flutter-bake";
        return false;
    }
    auto& layer = retained_flutter_;
    const DXGI_FORMAT surface_format = retained_surface_format(target);
    const auto log_failure = [&](const char* stage, HRESULT result) {
        retained_graph_fallback_reason_ = stage;
        spdlog::warn(
            "[WindowsNativeCompositor] retained flutter {} failed hr=0x{:08x}",
            stage,
            static_cast<uint32_t>(result));
        return false;
    };
    HRESULT hr = S_OK;
    if (!layer.surface || layer.width != surface_info.width ||
        layer.height != surface_info.height ||
        layer.format != surface_format) {
        layer.surface.Reset();
        layer.width = surface_info.width;
        layer.height = surface_info.height;
        layer.format = surface_format;
        hr = dcomp_device_->CreateSurface(
            layer.width, layer.height, surface_format,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &layer.surface);
        if (FAILED(hr)) return log_failure("create-surface", hr);
        hr = layer.visual->SetContent(layer.surface.Get());
        if (FAILED(hr)) return log_failure("set-content", hr);
    }

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    POINT offset = {};
    RECT rect = {
        0, 0,
        static_cast<LONG>(layer.width),
        static_cast<LONG>(layer.height)};
    hr = layer.surface->BeginDraw(&rect, IID_PPV_ARGS(&surface), &offset);
    if (FAILED(hr)) return log_failure("begin-draw", hr);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    bool ok = SUCCEEDED(surface.As(&texture)) &&
              SUCCEEDED(device_->CreateRenderTargetView(
                  texture.Get(), nullptr, &rtv));
    if (ok) {
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(layer.width);
        viewport.Height = static_cast<float>(layer.height);
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        ID3D11RenderTargetView* raw_rtv = rtv.Get();
        context_->OMSetRenderTargets(1, &raw_rtv, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        std::array<ID3D11ShaderResourceView*, 7> srvs = {};
        srvs[1] = flutter_srv;
        context_->PSSetShaderResources(
            0, static_cast<UINT>(srvs.size()), srvs.data());
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        CompositeConstants values = {};
        values.sdr_white_scale = static_cast<float>(
            sdr_white_scale_.load(std::memory_order_relaxed));
        values.output_mode = target == OutputTarget::ScRGB ? 1.0f : 0.0f;
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
        ID3D11Buffer* constants = constants_.Get();
        context_->PSSetConstantBuffers(0, 1, &constants);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context_->PSSetShader(flutter_pixel_shader_.Get(), nullptr, 0);
        context_->Draw(4, 0);
        std::array<ID3D11ShaderResourceView*, 7> null_srvs = {};
        context_->PSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
    }
    const HRESULT end_result = layer.surface->EndDraw();
    if (!ok) return log_failure("render-target-view", E_FAIL);
    if (FAILED(end_result)) return log_failure("end-draw", end_result);
    layer.generation = surface_info.frame_generation;
    layer.ready = true;
    ++retained_graph_flutter_bake_count_;
    retained_graph_fallback_reason_ = "none";
    return true;
}

bool WindowsNativeCompositor::ApplyRetainedProjection(
    uint32_t width,
    uint32_t height,
    OutputTarget target,
    const SourceProjection& projection) {
    if (!EnsureRetainedGraph(width, height, target)) {
        return false;
    }
    std::array<bool, 4> source_present{};
    for (size_t i = 0; i < retained_sources_.size(); ++i) {
        source_present[i] = retained_sources_[i].ready;
    }
    double viewport_values[4] = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < 4; ++i) {
            viewport_values[i] = viewport_[i];
        }
    }
    const auto rects = vr::project_windows_retained_source_visuals(
        static_cast<float>(viewport_values[0] * width),
        static_cast<float>(viewport_values[1] * height),
        static_cast<float>(viewport_values[2] * width),
        static_cast<float>(viewport_values[3] * height),
        projection,
        source_present);
    for (size_t slot = 0; slot < retained_sources_.size(); ++slot) {
        auto& layer = retained_sources_[slot];
        if (!layer.visual) {
            continue;
        }
        const auto& rect = rects[slot];
        if (!rect.present || !layer.ready ||
            std::fabs(rect.right - rect.left) < 0.001f ||
            std::fabs(rect.bottom - rect.top) < 0.001f) {
            layer.visual->SetOffsetX(-100000.0f);
            layer.visual->SetOffsetY(-100000.0f);
            continue;
        }
        const float scale_x =
            (rect.right - rect.left) / std::max(1.0f, static_cast<float>(layer.width));
        const float scale_y =
            (rect.bottom - rect.top) / std::max(1.0f, static_cast<float>(layer.height));
        const D2D_MATRIX_3X2_F transform = {
            scale_x, 0.0f,
            0.0f, scale_y,
            0.0f, 0.0f};
        layer.visual->SetTransform(transform);
        layer.visual->SetOffsetX(rect.left);
        layer.visual->SetOffsetY(rect.top);
        if (layer.clip) {
            layer.clip->SetLeft((rect.clip_left - rect.left) / scale_x);
            layer.clip->SetTop((rect.clip_top - rect.top) / scale_y);
            layer.clip->SetRight((rect.clip_right - rect.left) / scale_x);
            layer.clip->SetBottom((rect.clip_bottom - rect.top) / scale_y);
        }
    }
    if (retained_flutter_.visual && retained_flutter_.ready) {
        const float scale_x =
            static_cast<float>(std::max(width, 1u)) /
            std::max(1.0f, static_cast<float>(retained_flutter_.width));
        const float scale_y =
            static_cast<float>(std::max(height, 1u)) /
            std::max(1.0f, static_cast<float>(retained_flutter_.height));
        const D2D_MATRIX_3X2_F transform = {
            scale_x, 0.0f,
            0.0f, scale_y,
            0.0f, 0.0f};
        retained_flutter_.visual->SetTransform(transform);
        retained_flutter_.visual->SetOffsetX(0.0f);
        retained_flutter_.visual->SetOffsetY(0.0f);
    }
    retained_graph_fallback_reason_ = "none";
    return true;
}

bool WindowsNativeCompositor::ShouldDeferRetainedGraphCommitLocked(
    std::chrono::steady_clock::time_point now,
    bool projection_only) {
    if (last_retained_graph_commit_time_.time_since_epoch().count() == 0) {
        retained_graph_commit_deadline_ = {};
        return false;
    }
    const auto min_interval = std::chrono::microseconds(
        retained_graph_commit_interval_us(
            diagnostics_.high_refresh_display_hz,
            projection_only));
    const auto deadline = last_retained_graph_commit_time_ + min_interval;
    if (now >= deadline) {
        retained_graph_commit_deadline_ = {};
        return false;
    }
    if (retained_graph_commit_deadline_ != deadline) {
        retained_graph_commit_deadline_ = deadline;
        ++retained_graph_commit_defer_count_;
    }
    return true;
}

void WindowsNativeCompositor::RecordInteractionCommitLatencyLocked(
    std::chrono::steady_clock::time_point committed_at) {
    if (!interaction_sample_active_ || !retained_projection_dirty_) {
        return;
    }
    if (last_retained_projection_update_.time_since_epoch().count() == 0 ||
        committed_at < last_retained_projection_update_) {
        return;
    }
    high_refresh_metrics_.record_interaction_input_to_present_us(
        static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                committed_at - last_retained_projection_update_)
                .count()));
}

bool WindowsNativeCompositor::CommitRetainedGraph(const char* reason) {
    if (!retained_root_visual_ || !dcomp_target_ || !dcomp_device_) {
        retained_graph_fallback_reason_ = "retained-root-unavailable";
        return false;
    }
    HRESULT hr = S_OK;
    if (!retained_graph_active_) {
        hr = dcomp_target_->SetRoot(retained_root_visual_.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = dcomp_device_->Commit();
    }
    if (FAILED(hr)) {
        retained_graph_fallback_reason_ = "retained-commit-failed";
        spdlog::warn(
            "[WindowsNativeCompositor] retained commit failed reason={} hr=0x{:08x}",
            reason ? reason : "unspecified",
            static_cast<uint32_t>(hr));
        return false;
    }
    retained_graph_active_ = true;
    last_retained_graph_commit_time_ = std::chrono::steady_clock::now();
    retained_graph_commit_deadline_ = {};
    ++retained_graph_commit_count_;
    if (reason && std::strcmp(reason, "projection-only") == 0) {
        ++retained_graph_projection_commit_count_;
        ++retained_graph_projection_skip_present_count_;
    }
    retained_graph_fallback_reason_ = "none";
    return true;
}

bool WindowsNativeCompositor::CreatePipeline() {
    const auto log_failure = [](const char* stage, HRESULT result) {
        spdlog::error(
            "[WindowsNativeCompositor] {} failed hr=0x{:08x}",
            stage,
            static_cast<uint32_t>(result));
        return false;
    };
    const auto log_compile_failure =
        [](const char* stage,
           HRESULT result,
           const Microsoft::WRL::ComPtr<ID3DBlob>& errors) {
            const char* detail =
                errors && errors->GetBufferPointer()
                    ? static_cast<const char*>(errors->GetBufferPointer())
                    : "no compiler diagnostics";
            spdlog::error(
                "[WindowsNativeCompositor] {} failed hr=0x{:08x}: {}",
                stage,
                static_cast<uint32_t>(result),
                detail);
            return false;
        };
    const char* shader = vr::windows_dcomp_composite_hlsl();
    const size_t shader_size = std::strlen(shader);
    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> video_ps_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> flutter_ps_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> overlay_vs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> overlay_ps_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, &vs_blob, &errors);
    if (FAILED(hr)) return log_compile_failure("compile VSMain", hr, errors);
    errors.Reset();
    hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr)) return log_compile_failure("compile PSMain", hr, errors);
    errors.Reset();
    hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "VSOverlay", "vs_5_0", 0, 0, &overlay_vs_blob, &errors);
    if (FAILED(hr)) return log_compile_failure("compile VSOverlay", hr, errors);
    errors.Reset();
    hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "PSOverlay", "ps_5_0", 0, 0, &overlay_ps_blob, &errors);
    if (FAILED(hr)) return log_compile_failure("compile PSOverlay", hr, errors);
    errors.Reset();
    hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "PSVideo", "ps_5_0", 0, 0, &video_ps_blob, &errors);
    if (FAILED(hr)) return log_compile_failure("compile PSVideo", hr, errors);
    errors.Reset();
    hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "PSFlutter", "ps_5_0", 0, 0, &flutter_ps_blob, &errors);
    if (FAILED(hr)) return log_compile_failure("compile PSFlutter", hr, errors);
    hr = device_->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
            nullptr, &vertex_shader_);
    if (FAILED(hr)) return log_failure("CreateVertexShader", hr);
    hr = device_->CreatePixelShader(
            ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
            nullptr, &pixel_shader_);
    if (FAILED(hr)) return log_failure("CreatePixelShader PSMain", hr);
    hr = device_->CreatePixelShader(
            video_ps_blob->GetBufferPointer(),
            video_ps_blob->GetBufferSize(),
            nullptr,
            &video_pixel_shader_);
    if (FAILED(hr)) return log_failure("CreatePixelShader PSVideo", hr);
    hr = device_->CreatePixelShader(
            flutter_ps_blob->GetBufferPointer(),
            flutter_ps_blob->GetBufferSize(),
            nullptr,
            &flutter_pixel_shader_);
    if (FAILED(hr)) return log_failure("CreatePixelShader PSFlutter", hr);
    hr = device_->CreateVertexShader(
            overlay_vs_blob->GetBufferPointer(),
            overlay_vs_blob->GetBufferSize(),
            nullptr,
            &overlay_vertex_shader_);
    if (FAILED(hr)) return log_failure("CreateVertexShader VSOverlay", hr);
    hr = device_->CreatePixelShader(
            overlay_ps_blob->GetBufferPointer(),
            overlay_ps_blob->GetBufferSize(),
            nullptr,
            &overlay_pixel_shader_);
    if (FAILED(hr)) return log_failure("CreatePixelShader PSOverlay", hr);
    const D3D11_INPUT_ELEMENT_DESC overlay_elements[] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 8,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device_->CreateInputLayout(
            overlay_elements,
            static_cast<UINT>(std::size(overlay_elements)),
            overlay_vs_blob->GetBufferPointer(),
            overlay_vs_blob->GetBufferSize(),
            &overlay_input_layout_))) {
        return false;
    }
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sampler_desc, &sampler_))) return false;
    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha =
        D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(
            &blend_desc, &premultiplied_blend_state_))) {
        return false;
    }
    if (FAILED(device_->CreateBlendState(
            &blend_desc, &overlay_blend_state_))) {
        return false;
    }
    D3D11_BUFFER_DESC constants_desc = {};
    constants_desc.ByteWidth = sizeof(CompositeConstants);
    constants_desc.Usage = D3D11_USAGE_DEFAULT;
    constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device_->CreateBuffer(
        &constants_desc, nullptr, &constants_));
}

void WindowsNativeCompositor::ThreadMain() {
    const auto first_present_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true) {
        bool first_present_timed_out = false;
        bool flutter_export_stale_timed_out = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto wait_deadline = std::chrono::steady_clock::time_point::max();
            if (diagnostics_.present_count == 0) {
                wait_deadline = first_present_deadline;
            }
            if (pending_flutter_frame_request_sequence_ > 0) {
                const auto export_deadline =
                    pending_flutter_frame_request_time_ +
                    kFlutterExportStaleTimeout;
                wait_deadline = std::min(wait_deadline, export_deadline);
            }
            if (retained_deferred_content_deadline_
                    .time_since_epoch()
                    .count() != 0) {
                wait_deadline = std::min(
                    wait_deadline, retained_deferred_content_deadline_);
            }
            if (retained_graph_commit_deadline_
                    .time_since_epoch()
                    .count() != 0) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= retained_graph_commit_deadline_) {
                    retained_graph_commit_deadline_ = {};
                } else {
                    wait_deadline = std::min(
                        wait_deadline, retained_graph_commit_deadline_);
                }
            }
            if (wait_deadline !=
                std::chrono::steady_clock::time_point::max()) {
                wake_.wait_until(
                    lock, wait_deadline,
                    [this]() { return stop_ || work_pending_; });
            } else {
                wake_.wait(
                    lock, [this]() { return stop_ || work_pending_; });
            }
            if (stop_) {
                spdlog::info(
                    "[WindowsNativeCompositor] composition thread observed stop");
                return;
            }
            if (terminal_inactive_) return;
            first_present_timed_out =
                phase_ == Phase::Inactive &&
                diagnostics_.present_count == 0 &&
                std::chrono::steady_clock::now() >= first_present_deadline;
            flutter_export_stale_timed_out =
                !first_present_timed_out &&
                pending_flutter_frame_request_sequence_ > 0 &&
                std::chrono::steady_clock::now() >=
                    pending_flutter_frame_request_time_ +
                    kFlutterExportStaleTimeout;
            work_pending_ = false;
        }
        if (first_present_timed_out) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                terminal_inactive_ = true;
                ++diagnostics_.failure_count;
                diagnostics_.fallback_reason = "first-dcomp-present-timeout";
            }
            PublishState(Phase::Failed, "first-dcomp-present-timeout");
            return;
        }
        if (flutter_export_stale_timed_out) {
            uint64_t request_sequence = 0;
            uint64_t base_generation = 0;
            std::string reason;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                request_sequence = pending_flutter_frame_request_sequence_;
                base_generation = pending_flutter_frame_request_base_generation_;
                reason = pending_flutter_frame_request_reason_;
                ++flutter_export_stale_timeout_count_;
                diagnostics_.flutter_export_stale_timeout_count =
                    flutter_export_stale_timeout_count_;
                pending_flutter_frame_request_sequence_ = 0;
                pending_flutter_frame_request_base_generation_ = 0;
                pending_flutter_frame_request_reason_.clear();
                pending_flutter_frame_request_time_ = {};
                pending_flutter_frame_request_acquire_logged_ = false;
            }
            spdlog::warn(
                "[WindowsCompositorDebug] flutter export observation timed "
                "out without a newer surface seq={} reason={} "
                "baseGeneration={}; keeping native compositor active",
                request_sequence,
                reason.empty() ? "unspecified" : reason,
                base_generation);
        }
        if (!CompositeLatest()) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++diagnostics_.drop_count;
            high_refresh_metrics_.record_drop();
        }
    }
}

bool WindowsNativeCompositor::CompositeLatest() {
    const auto composite_started = std::chrono::steady_clock::now();
    auto acquire_finished = composite_started;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_inactive_) {
            return false;
        }
    }
    if (engine_api_.get_state && flutter_view_) {
        FlutterSurfaceExportState export_state = {};
        export_state.struct_size = sizeof(export_state);
        if (engine_api_.get_state(flutter_view_, &export_state)) {
            std::lock_guard<std::mutex> lock(mutex_);
            diagnostics_.engine_export_frame_pump_available = true;
            diagnostics_.flutter_export_ring_generation =
                export_state.ring_generation;
            diagnostics_.flutter_export_state_generation =
                export_state.frame_generation;
            diagnostics_.flutter_export_publish_count =
                export_state.publish_count;
            diagnostics_.flutter_export_request_count =
                export_state.request_count;
            diagnostics_.flutter_export_request_dispatch_count =
                export_state.request_dispatch_count;
            diagnostics_.flutter_export_schedule_frame_count =
                export_state.schedule_frame_count;
            diagnostics_.flutter_export_vsync_count =
                export_state.vsync_count;
            diagnostics_.flutter_export_present_count =
                export_state.present_count;
            diagnostics_.flutter_export_begin_count =
                export_state.export_begin_count;
            diagnostics_.flutter_export_begin_fail_count =
                export_state.export_begin_fail_count;
            diagnostics_.flutter_export_make_current_fail_count =
                export_state.export_make_current_fail_count;
            diagnostics_.flutter_export_publish_fail_count =
                export_state.export_publish_fail_count;
            diagnostics_.flutter_export_flush_count =
                export_state.export_flush_count;
            diagnostics_.flutter_export_finish_count =
                export_state.export_finish_count;
            diagnostics_.flutter_export_backpressure_count =
                export_state.backpressure_count;
            diagnostics_.flutter_export_pending_frame_pump_frames =
                export_state.pending_frame_pump_frames;
            diagnostics_.flutter_export_latest_available =
                export_state.latest_available;
            const auto now = std::chrono::steady_clock::now();
            if (last_flutter_export_pacing_log_.time_since_epoch().count() ==
                    0 ||
                now - last_flutter_export_pacing_log_ >=
                    kFlutterExportPacingSampleInterval) {
                const uint64_t now_us = steady_micros();
                const uint64_t request_delta = counter_delta(
                    export_state.request_count,
                    last_flutter_export_pacing_request_count_);
                const uint64_t request_dispatch_delta = counter_delta(
                    export_state.request_dispatch_count,
                    last_flutter_export_pacing_request_dispatch_count_);
                const uint64_t schedule_frame_delta = counter_delta(
                    export_state.schedule_frame_count,
                    last_flutter_export_pacing_schedule_frame_count_);
                const uint64_t vsync_delta = counter_delta(
                    export_state.vsync_count,
                    last_flutter_export_pacing_vsync_count_);
                const uint64_t publish_delta = counter_delta(
                    export_state.publish_count,
                    last_flutter_export_pacing_publish_count_);
                const uint64_t present_delta = counter_delta(
                    export_state.present_count,
                    last_flutter_export_pacing_present_count_);
                const uint64_t acquire_delta = counter_delta(
                    export_state.acquire_count,
                    last_flutter_export_pacing_acquire_count_);
                const uint64_t release_delta = counter_delta(
                    export_state.release_count,
                    last_flutter_export_pacing_release_count_);
                const uint64_t begin_delta = counter_delta(
                    export_state.export_begin_count,
                    last_flutter_export_pacing_begin_count_);
                const uint64_t backpressure_delta = counter_delta(
                    export_state.backpressure_count,
                    last_flutter_export_pacing_backpressure_count_);
                const uint64_t retained_delta = counter_delta(
                    retained_graph_commit_count_,
                    last_flutter_export_pacing_retained_commit_count_);
                const uint64_t retained_projection_delta = counter_delta(
                    retained_graph_projection_commit_count_,
                    last_flutter_export_pacing_retained_projection_count_);

                spdlog::debug(
                    "[WindowsCompositorDebug] flutter export pacing sample "
                    "requestDelta={} dispatchDelta={} scheduleDelta={} "
                    "vsyncDelta={} publishDelta={} presentDelta={} "
                    "acquireDelta={} releaseDelta={} beginDelta={} "
                    "backpressureDelta={} retainedDelta={} "
                    "retainedProjectionDelta={} agesMs request={} "
                    "dispatch={} schedule={} vsync={} begin={} publish={} "
                    "sync={} present={} acquire={} release={} backpressure={} "
                    "counts publish={} present={} acquire={} release={} "
                    "begin={} beginFail={} flush={} finish={} "
                    "backpressure={} generation={} "
                    "ring={} slot={} latest={} leases={} writingSlots={} "
                    "leasedSlots={} retiredRings={} latestSlotLease={} "
                    "pendingPump={} size={}x{}",
                    request_delta,
                    request_dispatch_delta,
                    schedule_frame_delta,
                    vsync_delta,
                    publish_delta,
                    present_delta,
                    acquire_delta,
                    release_delta,
                    begin_delta,
                    backpressure_delta,
                    retained_delta,
                    retained_projection_delta,
                    age_ms(now_us, export_state.last_request_time_us),
                    age_ms(now_us,
                           export_state.last_request_dispatch_time_us),
                    age_ms(now_us, export_state.last_schedule_frame_time_us),
                    age_ms(now_us, export_state.last_vsync_time_us),
                    age_ms(now_us, export_state.last_begin_time_us),
                    age_ms(now_us, export_state.last_publish_time_us),
                    age_ms(now_us, export_state.last_export_sync_time_us),
                    age_ms(now_us, export_state.last_present_time_us),
                    age_ms(now_us, export_state.last_acquire_time_us),
                    age_ms(now_us, export_state.last_release_time_us),
                    age_ms(now_us, export_state.last_backpressure_time_us),
                    export_state.publish_count,
                    export_state.present_count,
                    export_state.acquire_count,
                    export_state.release_count,
                    export_state.export_begin_count,
                    export_state.export_begin_fail_count,
                    export_state.export_flush_count,
                    export_state.export_finish_count,
                    export_state.backpressure_count,
                    export_state.frame_generation,
                    export_state.ring_generation,
                    export_state.latest_slot,
                    export_state.latest_available,
                    export_state.active_lease_count,
                    export_state.writing_slot_count,
                    export_state.leased_slot_count,
                    export_state.retired_ring_count,
                    export_state.latest_slot_lease_count,
                    export_state.pending_frame_pump_frames,
                    export_state.width,
                    export_state.height);
                last_flutter_export_pacing_log_ = now;
                last_flutter_export_pacing_request_count_ =
                    export_state.request_count;
                last_flutter_export_pacing_request_dispatch_count_ =
                    export_state.request_dispatch_count;
                last_flutter_export_pacing_schedule_frame_count_ =
                    export_state.schedule_frame_count;
                last_flutter_export_pacing_vsync_count_ =
                    export_state.vsync_count;
                last_flutter_export_pacing_publish_count_ =
                    export_state.publish_count;
                last_flutter_export_pacing_present_count_ =
                    export_state.present_count;
                last_flutter_export_pacing_acquire_count_ =
                    export_state.acquire_count;
                last_flutter_export_pacing_release_count_ =
                    export_state.release_count;
                last_flutter_export_pacing_begin_count_ =
                    export_state.export_begin_count;
                last_flutter_export_pacing_backpressure_count_ =
                    export_state.backpressure_count;
                last_flutter_export_pacing_retained_commit_count_ =
                    retained_graph_commit_count_;
                last_flutter_export_pacing_retained_projection_count_ =
                    retained_graph_projection_commit_count_;
            }
        }
    }
    auto player = player_.lock();
    if (!player) return false;
    Microsoft::WRL::ComPtr<IDXGIAdapter> pending_output_adapter;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_output_adapter = pending_output_adapter_;
        pending_output_adapter_.Reset();
    }
    if (pending_output_adapter) {
        ReleaseHeldInputs(player);
        if (!InitializeDeviceAndComposition(
                producer_adapter_.Get(), pending_output_adapter.Get()) ||
            !CreatePipeline()) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++diagnostics_.output_migration_failure_count;
            ++diagnostics_.device_recovery_failure_count;
            diagnostics_.cross_adapter_last_error =
                "output-adapter-reinitialize-failed";
            diagnostics_.device_recovery_state =
                vr::windows_device_recovery_state_name(
                    desired_output_target_ == OutputTarget::ScRGB
                        ? vr::WindowsDeviceRecoveryState::FallbackNativeSdr
                        : vr::WindowsDeviceRecoveryState::FailedTerminal);
            diagnostics_.device_recovery_fallback_stage =
                desired_output_target_ == OutputTarget::ScRGB
                    ? "native-sdr"
                    : "native-compositor-failed";
            pending_output_adapter_ = pending_output_adapter;
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++diagnostics_.output_migration_count;
            diagnostics_.output_adapter_luid =
                luid_string(output_luid_high_, output_luid_low_);
            diagnostics_.pending_output_adapter_luid =
                diagnostics_.output_adapter_luid;
            diagnostics_.cross_adapter_required = IsCrossAdapterActive();
            diagnostics_.cross_adapter_supported =
                !IsCrossAdapterActive() || transport_support_.bgra8;
            diagnostics_.cross_adapter_transport_mode =
                IsCrossAdapterActive() ? "row-major-gpu-copy"
                                       : "same-adapter";
            diagnostics_.cross_adapter_transport_status =
                IsCrossAdapterActive() ? transport_support_.status
                                       : "not-required";
            diagnostics_.cross_adapter_sync_kind =
                IsCrossAdapterActive() ? "event-query"
                                       : "keyed-mutex";
            diagnostics_.cross_adapter_requested_sync_kind =
                vr::windows_cross_adapter_sync_request_name(
                    cross_adapter_sync_request_);
            diagnostics_.cross_adapter_active_sync_kind =
                diagnostics_.cross_adapter_sync_kind;
            diagnostics_.cross_adapter_sync_fallback_reason = "none";
            diagnostics_.transport_bgra8_supported =
                transport_support_.bgra8;
            diagnostics_.transport_fp16_supported =
                transport_support_.rgba16f;
            diagnostics_.transport_shared_fence_supported =
                transport_support_.shared_fence;
            diagnostics_.transport_shared_fence_producer_supported =
                transport_support_.shared_fence_producer;
            diagnostics_.transport_shared_fence_output_supported =
                transport_support_.shared_fence_output;
            diagnostics_.transport_shared_fence_handle_created =
                transport_support_.shared_fence_handle_created;
            diagnostics_.transport_shared_fence_open_succeeded =
                transport_support_.shared_fence_open_succeeded;
            diagnostics_.transition_state = "preparing";
            if (diagnostics_.device_recovery_state ==
                vr::windows_device_recovery_state_name(
                    vr::WindowsDeviceRecoveryState::RebuildingPresentation)) {
                diagnostics_.device_recovery_state =
                    vr::windows_device_recovery_state_name(
                        vr::WindowsDeviceRecoveryState::Recovered);
                ++diagnostics_.device_recovery_success_count;
            }
        }
    }
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    if (FAILED(device_.As(&device1)) || !device1) {
        EnterFailed("dcomp-query-device1-failed");
        return false;
    }

    SourceProjection source_projection;
    bool retained_projection_only = false;
    bool retained_projection_deferred_content = false;
    bool retained_projection_deferred_content_expired = false;
    bool retained_commit_deferred = false;
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        source_projection = source_projection_;
        retained_projection_deferred_content =
            retained_source_content_dirty_ ||
            retained_flutter_content_dirty_;
        retained_projection_deferred_content_expired =
            retained_projection_deferred_content &&
            retained_deferred_content_deadline_
                    .time_since_epoch()
                    .count() != 0 &&
            now >= retained_deferred_content_deadline_;
        retained_projection_only =
            retained_graph_active_ &&
            retained_projection_dirty_ &&
            (!retained_projection_deferred_content ||
             !retained_projection_deferred_content_expired) &&
            phase_ == Phase::Active &&
            current_swap_chain_.swap_chain &&
            CanUseRetainedGraph(source_projection, current_swap_chain_.target);
        if (retained_projection_only) {
            retained_commit_deferred =
                ShouldDeferRetainedGraphCommitLocked(now, true);
        }
    }
    if (retained_commit_deferred) {
        return true;
    }
    int64_t retained_apply_us = 0;
    int64_t retained_commit_us = 0;
    if (retained_projection_only) {
        const auto apply_started = std::chrono::steady_clock::now();
        retained_projection_only = ApplyRetainedProjection(
            current_swap_chain_.width,
            current_swap_chain_.height,
            current_swap_chain_.target,
            source_projection);
        const auto apply_finished = std::chrono::steady_clock::now();
        retained_apply_us = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                apply_finished - apply_started)
                .count());
        if (retained_projection_only) {
            const auto commit_started = std::chrono::steady_clock::now();
            retained_projection_only = CommitRetainedGraph("projection-only");
            const auto commit_finished = std::chrono::steady_clock::now();
            retained_commit_us = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    commit_finished - commit_started)
                    .count());
        }
    }
    if (retained_projection_only) {
        const auto committed_at = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        retained_projection_dirty_ = false;
        if (retained_projection_deferred_content) {
            const int64_t display_hz = std::max<int64_t>(
                1, diagnostics_.high_refresh_display_hz);
            const auto defer_delay = std::chrono::microseconds(
                std::clamp<int64_t>(
                    1000000 / display_hz,
                    kMinRetainedDeferredContentDelayUs,
                    kMaxRetainedDeferredContentDelayUs));
            if (retained_deferred_content_deadline_
                    .time_since_epoch()
                    .count() == 0) {
                retained_deferred_content_deadline_ =
                    committed_at + defer_delay;
            }
            ++retained_graph_deferred_content_count_;
        } else {
            retained_deferred_content_deadline_ = {};
        }
        append_retained_graph_sample(
            retained_graph_apply_us_, retained_apply_us);
        append_retained_graph_sample(
            retained_graph_commit_us_, retained_commit_us);
        high_refresh_metrics_.record_composite_us(
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    committed_at - composite_started)
                    .count()));
        high_refresh_metrics_.record_draw_us(0);
        high_refresh_metrics_.record_present_block_us(0);
        high_refresh_metrics_.record_source_projection_reuse();
        if (overlay_layer_state_.snapshot().active) {
            overlay_layer_state_.reuse();
            overlay_layer_state_.composite();
            high_refresh_metrics_.record_overlay_layer_reuse();
            high_refresh_metrics_.record_overlay_composite_us(0);
        }
        RecordInteractionCommitLatencyLocked(committed_at);
        diagnostics_.source_projection_enabled = source_projection.enabled;
        diagnostics_.source_cache_active = true;
        diagnostics_.retained_graph_active = true;
        diagnostics_.retained_graph_mode = "projection-only";
        diagnostics_.retained_graph_fallback_reason = "none";
        diagnostics_.retained_graph_commit_count =
            retained_graph_commit_count_;
        diagnostics_.retained_graph_projection_commit_count =
            retained_graph_projection_commit_count_;
        diagnostics_.retained_graph_source_bake_count =
            retained_graph_source_bake_count_;
        diagnostics_.retained_graph_flutter_bake_count =
            retained_graph_flutter_bake_count_;
        diagnostics_.retained_graph_projection_skip_present_count =
            retained_graph_projection_skip_present_count_;
        diagnostics_.retained_graph_deferred_content_count =
            retained_graph_deferred_content_count_;
        return true;
    }

    const auto release_held_video = [&]() {
        if (!held_video_valid_) return;
        if (held_video_mutex_) {
            held_video_mutex_->ReleaseSync(
                held_video_.producer_release_key);
        }
        player->release_shared_fp16_texture(
            held_video_.buffer_index, held_video_.ring_generation);
        held_video_valid_ = false;
        held_video_ = {};
        held_video_srv_.Reset();
        held_video_mutex_.Reset();
        held_video_texture_.Reset();
    };
    const auto release_held_sdr_video = [&]() {
        if (!held_sdr_video_valid_) return;
        player->release_shared_texture(
            held_sdr_video_.buffer_index,
            held_sdr_video_.buffer_generation);
        if (held_sdr_video_.texture) {
            static_cast<ID3D11Texture2D*>(
                held_sdr_video_.texture)->Release();
        }
        held_sdr_video_valid_ = false;
        held_sdr_video_ = {};
        held_sdr_video_srv_.Reset();
        held_sdr_video_texture_.Reset();
    };
    const auto release_held_flutter = [&]() {
        if (!held_flutter_valid_) return;
        if (held_flutter_mutex_) {
            held_flutter_mutex_->ReleaseSync(
                held_flutter_.producer_release_key);
        }
        engine_api_.release(flutter_view_, held_flutter_.lease_id);
        held_flutter_valid_ = false;
        held_flutter_ = {};
        held_flutter_srv_.Reset();
        held_flutter_mutex_.Reset();
        held_flutter_texture_.Reset();
    };
    const auto release_held_source = [&]() {
        if (!held_source_valid_) return;
        for (size_t slot = 0; slot < held_source_mutexes_.size(); ++slot) {
            if (held_source_present_[slot] && held_source_mutexes_[slot]) {
                held_source_mutexes_[slot]->ReleaseSync(0);
            }
        }
        player->release_source_cache_bundle(
            held_source_.buffer_index, held_source_.ring_generation);
        held_source_valid_ = false;
        held_source_ = {};
        held_source_present_.fill(false);
        held_source_srvs_ = {};
        held_source_mutexes_ = {};
        held_source_textures_ = {};
        held_source_transfer_.fill(0);
    };
    const auto log_pending_flutter_request_acquire =
        [&](const char* outcome, const FlutterSurface* surface) {
            uint64_t request_sequence = 0;
            uint64_t base_generation = 0;
            std::string reason;
            int64_t elapsed_ms = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_flutter_frame_request_sequence_ == 0 ||
                    pending_flutter_frame_request_acquire_logged_) {
                    return;
                }
                pending_flutter_frame_request_acquire_logged_ = true;
                request_sequence = pending_flutter_frame_request_sequence_;
                base_generation =
                    pending_flutter_frame_request_base_generation_;
                reason = pending_flutter_frame_request_reason_;
                elapsed_ms = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() -
                        pending_flutter_frame_request_time_)
                        .count());
            }
            if (surface) {
                spdlog::debug(
                    "[WindowsCompositorDebug] flutter acquire after request "
                    "seq={} reason={} outcome={} baseGeneration={} "
                    "latestGeneration={} ring={} slot={} elapsedMs={}",
                    request_sequence,
                    reason.empty() ? "unspecified" : reason,
                    outcome,
                    base_generation,
                    surface->frame_generation,
                    surface->ring_generation,
                    surface->slot,
                    elapsed_ms);
            } else {
                spdlog::debug(
                    "[WindowsCompositorDebug] flutter acquire after request "
                    "seq={} reason={} outcome={} baseGeneration={} "
                    "elapsedMs={}",
                    request_sequence,
                    reason.empty() ? "unspecified" : reason,
                    outcome,
                    base_generation,
                    elapsed_ms);
            }
        };
    const auto complete_pending_flutter_request =
        [&](const FlutterSurface& surface) {
            uint64_t request_sequence = 0;
            uint64_t base_generation = 0;
            std::string reason;
            int64_t elapsed_ms = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_flutter_frame_request_sequence_ == 0 ||
                    surface.frame_generation <=
                        pending_flutter_frame_request_base_generation_) {
                    return;
                }
                request_sequence = pending_flutter_frame_request_sequence_;
                base_generation =
                    pending_flutter_frame_request_base_generation_;
                reason = pending_flutter_frame_request_reason_;
                elapsed_ms = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() -
                        pending_flutter_frame_request_time_)
                        .count());
                pending_flutter_frame_request_sequence_ = 0;
                pending_flutter_frame_request_base_generation_ = 0;
                pending_flutter_frame_request_reason_.clear();
                pending_flutter_frame_request_time_ = {};
                pending_flutter_frame_request_acquire_logged_ = false;
            }
            spdlog::debug(
                "[WindowsCompositorDebug] flutter request satisfied "
                "seq={} reason={} baseGeneration={} acquiredGeneration={} "
                "ring={} slot={} elapsedMs={}",
                request_sequence,
                reason.empty() ? "unspecified" : reason,
                base_generation,
                surface.frame_generation,
                surface.ring_generation,
                surface.slot,
                elapsed_ms);
        };
    vr::SharedFp16TextureSnapshot next_video;
    if (player->acquire_shared_fp16_texture(next_video)) {
        const bool unchanged =
            held_video_valid_ &&
            next_video.ring_generation == held_video_.ring_generation &&
            next_video.frame_generation == held_video_.frame_generation;
        if (unchanged) {
            player->release_shared_fp16_texture(
                next_video.buffer_index, next_video.ring_generation);
        } else {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            bool acquired = false;
            const HRESULT open_result = OpenInputTexture(
                device1.Get(), next_video.handle, &texture)
                ? S_OK
                : E_FAIL;
            if (SUCCEEDED(open_result) &&
                SUCCEEDED(texture.As(&keyed_mutex))) {
                acquired =
                    keyed_mutex->AcquireSync(
                        next_video.consumer_acquire_key, 8) == S_OK;
            }
            bool srv_ready = false;
            if (acquired && IsCrossAdapterActive()) {
                srv_ready = TransportInput(
                    texture.Get(),
                    DXGI_FORMAT_R16G16B16A16_FLOAT,
                    static_cast<uint32_t>(next_video.width),
                    static_cast<uint32_t>(next_video.height),
                    video_transport_,
                    srv);
            } else if (acquired) {
                srv_ready = SUCCEEDED(device_->CreateShaderResourceView(
                    texture.Get(), nullptr, &srv));
            }
            if (srv_ready) {
                release_held_video();
                held_video_ = next_video;
                held_video_texture_ = std::move(texture);
                held_video_mutex_ = std::move(keyed_mutex);
                held_video_srv_ = std::move(srv);
                held_video_valid_ = true;
                if (IsCrossAdapterActive()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    diagnostics_.video_transport_generation =
                        diagnostics_.transport_generation;
                }
            } else {
                if (acquired) {
                    keyed_mutex->ReleaseSync(
                        next_video.producer_release_key);
                }
                player->release_shared_fp16_texture(
                    next_video.buffer_index, next_video.ring_generation);
            }
        }
    }

    vr::SharedTextureSnapshot next_sdr_video;
    if (player->acquire_shared_texture(next_sdr_video)) {
        const bool unchanged =
            held_sdr_video_valid_ &&
            next_sdr_video.buffer_generation ==
                held_sdr_video_.buffer_generation &&
            next_sdr_video.buffer_index ==
                held_sdr_video_.buffer_index;
        if (unchanged) {
            player->release_shared_texture(
                next_sdr_video.buffer_index,
                next_sdr_video.buffer_generation);
            if (next_sdr_video.texture) {
                static_cast<ID3D11Texture2D*>(
                    next_sdr_video.texture)->Release();
            }
        } else {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            const HRESULT open_result = IsCrossAdapterActive()
                ? producer_device_->OpenSharedResource(
                      next_sdr_video.handle, IID_PPV_ARGS(&texture))
                : device_->OpenSharedResource(
                      next_sdr_video.handle, IID_PPV_ARGS(&texture));
            bool srv_ready = false;
            if (SUCCEEDED(open_result) && IsCrossAdapterActive()) {
                D3D11_TEXTURE2D_DESC desc = {};
                texture->GetDesc(&desc);
                srv_ready = TransportInput(
                    texture.Get(),
                    desc.Format,
                    desc.Width,
                    desc.Height,
                    sdr_video_transport_,
                    srv);
            } else if (SUCCEEDED(open_result)) {
                srv_ready = SUCCEEDED(device_->CreateShaderResourceView(
                    texture.Get(), nullptr, &srv));
            }
            if (srv_ready) {
                release_held_sdr_video();
                held_sdr_video_ = next_sdr_video;
                held_sdr_video_texture_ = std::move(texture);
                held_sdr_video_srv_ = std::move(srv);
                held_sdr_video_valid_ = true;
            } else {
                player->release_shared_texture(
                    next_sdr_video.buffer_index,
                    next_sdr_video.buffer_generation);
                if (next_sdr_video.texture) {
                    static_cast<ID3D11Texture2D*>(
                        next_sdr_video.texture)->Release();
                }
            }
        }
    }

    FlutterSurface next_flutter;
    if (engine_api_.acquire(flutter_view_, &next_flutter)) {
        const bool unchanged =
            held_flutter_valid_ &&
            next_flutter.ring_generation ==
                held_flutter_.ring_generation &&
            next_flutter.frame_generation ==
                held_flutter_.frame_generation;
        if (unchanged) {
            log_pending_flutter_request_acquire(
                "unchanged-latest-surface", &next_flutter);
            engine_api_.release(flutter_view_, next_flutter.lease_id);
        } else {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            bool acquired = false;
            const HRESULT open_result = OpenInputTexture(
                device1.Get(),
                next_flutter.shared_texture_handle,
                &texture)
                ? S_OK
                : E_FAIL;
            if (SUCCEEDED(open_result) &&
                SUCCEEDED(texture.As(&keyed_mutex))) {
                acquired =
                    keyed_mutex->AcquireSync(
                        next_flutter.consumer_acquire_key, 8) == S_OK;
            }
            bool srv_ready = false;
            if (acquired && IsCrossAdapterActive()) {
                srv_ready = TransportInput(
                    texture.Get(),
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    next_flutter.width,
                    next_flutter.height,
                    flutter_transport_,
                    srv);
            } else if (acquired) {
                srv_ready = SUCCEEDED(device_->CreateShaderResourceView(
                    texture.Get(), nullptr, &srv));
            }
            if (srv_ready) {
                release_held_flutter();
                held_flutter_ = next_flutter;
                held_flutter_texture_ = std::move(texture);
                held_flutter_mutex_ = std::move(keyed_mutex);
                held_flutter_srv_ = std::move(srv);
                held_flutter_valid_ = true;
                complete_pending_flutter_request(held_flutter_);
                ++flutter_generation_log_count_;
                if (flutter_generation_log_count_ <= 8 ||
                    flutter_generation_log_count_ % 60 == 0) {
                    spdlog::debug(
                        "[WindowsCompositorDebug] dcomp acquired flutter "
                        "surface generation={} ring={} slot={} size={}x{} "
                        "lease={}",
                        held_flutter_.frame_generation,
                        held_flutter_.ring_generation,
                        held_flutter_.slot,
                        held_flutter_.width,
                        held_flutter_.height,
                        held_flutter_.lease_id);
                }
                if (IsCrossAdapterActive()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    diagnostics_.flutter_transport_generation =
                        diagnostics_.transport_generation;
                }
            } else {
                log_pending_flutter_request_acquire(
                    acquired ? "srv-create-failed" :
                               "open-or-keyed-mutex-acquire-failed",
                    &next_flutter);
                if (acquired) {
                    keyed_mutex->ReleaseSync(
                        next_flutter.producer_release_key);
                }
                engine_api_.release(
                    flutter_view_, next_flutter.lease_id);
            }
        }
    } else {
        log_pending_flutter_request_acquire("acquire-latest-failed", nullptr);
    }

    if (!held_flutter_valid_) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (source_cache_publish_count_ > 0 &&
            source_projection.enabled &&
            !source_cache_base_lease_wait_logged_) {
            source_cache_base_lease_wait_logged_ = true;
            spdlog::info(
                "[WindowsNativeCompositor] source cache waiting for "
                "stable video/Flutter inputs");
        }
        return false;
    }
    if (!EnsureSwapChain(
            held_flutter_.width, held_flutter_.height)) {
        EnterFailed("dcomp-swap-chain-create-or-resize-failed");
        return false;
    }
    const OutputTarget composite_target =
        pending_swap_chain_.swap_chain
            ? pending_swap_chain_.target
            : current_swap_chain_.target;
    const bool needs_fp16_video = composite_target == OutputTarget::ScRGB;
    const bool needs_sdr_video = composite_target == OutputTarget::SDR;
    if ((needs_fp16_video && !held_video_valid_) ||
        (needs_sdr_video && !held_sdr_video_valid_)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (source_cache_publish_count_ > 0 &&
            source_projection.enabled &&
            !source_cache_base_lease_wait_logged_) {
            source_cache_base_lease_wait_logged_ = true;
            spdlog::info(
                "[WindowsNativeCompositor] source cache waiting for "
                "stable video/Flutter inputs");
        }
        return false;
    }

    if (!source_projection.enabled) {
        release_held_source();
    } else {
        vr::SharedSourceCacheBundleSnapshot next_source;
        const bool acquired =
            player->acquire_source_cache_bundle(next_source);
        if (acquired) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!source_cache_bundle_acquire_logged_) {
                    source_cache_bundle_acquire_logged_ = true;
                    spdlog::info(
                        "[WindowsNativeCompositor] first source cache bundle "
                        "acquired generation={} textures={}",
                        next_source.frame_generation,
                        next_source.texture_count);
                }
            }
            const bool unchanged =
                held_source_valid_ &&
                next_source.ring_generation ==
                    held_source_.ring_generation &&
                next_source.frame_generation ==
                    held_source_.frame_generation;
            if (unchanged) {
                player->release_source_cache_bundle(
                    next_source.buffer_index,
                    next_source.ring_generation);
            } else {
                std::array<
                    Microsoft::WRL::ComPtr<ID3D11Texture2D>, 4>
                    textures;
                std::array<
                    Microsoft::WRL::ComPtr<IDXGIKeyedMutex>, 4>
                    mutexes;
                std::array<
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4>
                    srvs;
                std::array<bool, 4> present{};
                std::array<int, 4> transfers{};
                bool complete = next_source.texture_count > 0;
                std::string error = "source-cache-invalid-texture-snapshot";
                for (size_t i = 0;
                     complete && i < next_source.texture_count;
                     ++i) {
                    const auto& source = next_source.textures[i];
                    const int slot = source.source_slot;
                    if (slot < 0 || slot >= 4 || !source.handle) {
                        complete = false;
                        break;
                    }
                    HRESULT result = OpenInputTexture(
                        device1.Get(), source.handle, &textures[slot])
                        ? S_OK
                        : E_FAIL;
                    if (FAILED(result)) {
                        error =
                            "source-cache-open-shared-resource-failed";
                        complete = false;
                        break;
                    }
                    result = textures[slot].As(&mutexes[slot]);
                    if (FAILED(result)) {
                        error = "source-cache-keyed-mutex-query-failed";
                        complete = false;
                        break;
                    }
                    result = mutexes[slot]->AcquireSync(
                        source.consumer_acquire_key, 8);
                    if (result != S_OK) {
                        error = "source-cache-keyed-mutex-timeout";
                        complete = false;
                        break;
                    }
                    present[slot] = true;
                    transfers[slot] = source.color_transfer;
                    bool srv_ready = false;
                    if (IsCrossAdapterActive()) {
                        srv_ready = TransportInput(
                            textures[slot].Get(),
                            DXGI_FORMAT_R16G16B16A16_FLOAT,
                            static_cast<uint32_t>(source.width),
                            static_cast<uint32_t>(source.height),
                            source_transports_[slot],
                            srvs[slot]);
                    } else {
                        result = device_->CreateShaderResourceView(
                            textures[slot].Get(), nullptr, &srvs[slot]);
                        srv_ready = SUCCEEDED(result);
                    }
                    if (!srv_ready) {
                        error = "source-cache-srv-creation-failed";
                        complete = false;
                        break;
                    }
                }
                if (complete) {
                    release_held_source();
                    held_source_ = std::move(next_source);
                    held_source_textures_ = std::move(textures);
                    held_source_mutexes_ = std::move(mutexes);
                    held_source_srvs_ = std::move(srvs);
                    held_source_present_ = present;
                    held_source_transfer_ = transfers;
                    held_source_valid_ = true;
                    if (IsCrossAdapterActive()) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        diagnostics_.source_transport_generation =
                            diagnostics_.transport_generation;
                    }
                } else {
                    for (size_t slot = 0; slot < present.size(); ++slot) {
                        if (present[slot] && mutexes[slot]) {
                            mutexes[slot]->ReleaseSync(0);
                        }
                    }
                    player->release_source_cache_bundle(
                        next_source.buffer_index,
                        next_source.ring_generation);
                    std::lock_guard<std::mutex> lock(mutex_);
                    source_cache_error_ = error;
                    diagnostics_.source_cache_last_error =
                        source_cache_error_;
                    ++diagnostics_.source_cache_fallback_count;
                }
            }
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            if (source_cache_publish_count_ > 0 &&
                !held_source_valid_ &&
                source_cache_error_ !=
                    "source-cache-bundle-acquire-failed") {
                source_cache_error_ =
                    "source-cache-bundle-acquire-failed";
                diagnostics_.source_cache_last_error =
                    source_cache_error_;
                ++diagnostics_.source_cache_fallback_count;
            }
        }
    }

    const bool source_bundle_active =
        source_projection.enabled && held_source_valid_;
    acquire_finished = std::chrono::steady_clock::now();
    bool retained_graph_supported = false;
    bool retained_graph_committed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retained_graph_supported =
            phase_ == Phase::Active &&
            source_bundle_active &&
            CanUseRetainedGraph(source_projection, composite_target);
    }
    bool retained_content_deferred = false;
    bool retained_content_commit_deferred = false;
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        const bool content_dirty =
            retained_source_content_dirty_ ||
            retained_flutter_content_dirty_;
        const bool deferred_deadline_active =
            retained_deferred_content_deadline_
                    .time_since_epoch()
                    .count() != 0;
        const bool deferred_deadline_expired =
            deferred_deadline_active &&
            now >= retained_deferred_content_deadline_;
        const bool projection_recent =
            last_retained_projection_update_.time_since_epoch().count() != 0 &&
            now - last_retained_projection_update_ <=
                kRetainedProjectionInteractionWindow;
        retained_content_deferred =
            retained_graph_supported &&
            retained_graph_active_ &&
            content_dirty &&
            !retained_projection_dirty_ &&
            projection_recent &&
            pending_flutter_frame_request_sequence_ == 0 &&
            !deferred_deadline_expired;
        if (!retained_content_deferred && retained_graph_supported &&
            retained_graph_active_) {
            retained_content_commit_deferred =
                ShouldDeferRetainedGraphCommitLocked(now, false);
        }
        if (retained_content_deferred) {
            const int64_t display_hz = std::max<int64_t>(
                1, diagnostics_.high_refresh_display_hz);
            const auto defer_delay = std::chrono::microseconds(
                std::clamp<int64_t>(
                    1000000 / display_hz,
                    kMinRetainedDeferredContentDelayUs,
                    kMaxRetainedDeferredContentDelayUs));
            if (!deferred_deadline_active) {
                retained_deferred_content_deadline_ = now + defer_delay;
            }
            ++retained_graph_deferred_content_count_;
            append_retained_graph_sample(retained_graph_apply_us_, 0);
            append_retained_graph_sample(retained_graph_commit_us_, 0);
            high_refresh_metrics_.record_composite_us(0);
            high_refresh_metrics_.record_draw_us(0);
            high_refresh_metrics_.record_present_block_us(0);
            high_refresh_metrics_.record_source_projection_reuse();
            diagnostics_.flutter_generation = held_flutter_.frame_generation;
            diagnostics_.video_generation = held_source_.frame_generation;
            diagnostics_.source_projection_enabled = true;
            diagnostics_.source_cache_active = true;
            diagnostics_.source_cache_consumed_generation =
                held_source_.frame_generation;
            diagnostics_.retained_graph_active = true;
            diagnostics_.retained_graph_mode = "content-deferred";
            diagnostics_.retained_graph_fallback_reason = "none";
            diagnostics_.retained_graph_commit_count =
                retained_graph_commit_count_;
            diagnostics_.retained_graph_projection_commit_count =
                retained_graph_projection_commit_count_;
            diagnostics_.retained_graph_source_bake_count =
                retained_graph_source_bake_count_;
            diagnostics_.retained_graph_flutter_bake_count =
                retained_graph_flutter_bake_count_;
            diagnostics_.retained_graph_projection_skip_present_count =
                retained_graph_projection_skip_present_count_;
            diagnostics_.retained_graph_deferred_content_count =
                retained_graph_deferred_content_count_;
            diagnostics_.retained_graph_commit_defer_count =
                retained_graph_commit_defer_count_;
        }
    }
    if (retained_content_deferred) {
        return true;
    }
    if (retained_content_commit_deferred) {
        return true;
    }
    if (retained_graph_supported) {
        bool retained_ok = true;
        bool retained_flutter_baked = false;
        bool retained_source_baked = false;
        int64_t retained_flutter_bake_us = 0;
        int64_t retained_source_bake_us = 0;
        int64_t retained_apply_us = 0;
        int64_t retained_commit_us = 0;
        auto retained_draw_finished = acquire_finished;
        if (!retained_flutter_.ready ||
            retained_flutter_.generation != held_flutter_.frame_generation ||
            retained_flutter_.format != retained_surface_format(composite_target)) {
            retained_flutter_baked = true;
            const auto bake_started = std::chrono::steady_clock::now();
            retained_ok = BakeRetainedFlutterSurface(
                held_flutter_, composite_target, held_flutter_srv_.Get());
            const auto bake_finished = std::chrono::steady_clock::now();
            retained_flutter_bake_us = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    bake_finished - bake_started)
                    .count());
        }
        for (size_t slot = 0; slot < retained_sources_.size(); ++slot) {
            if (!held_source_present_[slot]) {
                retained_sources_[slot].ready = false;
                retained_sources_[slot].generation = 0;
            }
        }
        for (size_t i = 0; retained_ok && i < held_source_.texture_count; ++i) {
            const auto& source = held_source_.textures[i];
            const int slot = source.source_slot;
            if (slot < 0 || slot >= 4 || !held_source_present_[slot]) {
                continue;
            }
            const auto& layer = retained_sources_[static_cast<size_t>(slot)];
            if (!layer.ready ||
                layer.generation != held_source_.frame_generation ||
                layer.width != static_cast<uint32_t>(source.width) ||
                layer.height != static_cast<uint32_t>(source.height) ||
                layer.format != retained_surface_format(composite_target)) {
                retained_source_baked = true;
                const auto bake_started = std::chrono::steady_clock::now();
                retained_ok = BakeRetainedSourceSurface(
                    static_cast<size_t>(slot),
                    held_source_srvs_[static_cast<size_t>(slot)].Get(),
                    static_cast<uint32_t>(source.width),
                    static_cast<uint32_t>(source.height),
                    composite_target,
                    source.color_transfer,
                    held_source_.frame_generation,
                    held_source_.overlay);
                const auto bake_finished = std::chrono::steady_clock::now();
                retained_source_bake_us += static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        bake_finished - bake_started)
                        .count());
            }
        }
        if (retained_ok) {
            const auto apply_started = std::chrono::steady_clock::now();
            retained_ok = ApplyRetainedProjection(
                held_flutter_.width,
                held_flutter_.height,
                composite_target,
                source_projection);
            const auto apply_finished = std::chrono::steady_clock::now();
            retained_apply_us = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    apply_finished - apply_started)
                    .count());
        }
        if (retained_ok) {
            const auto commit_started = std::chrono::steady_clock::now();
            retained_draw_finished = commit_started;
            retained_graph_committed = CommitRetainedGraph("content-update");
            const auto commit_finished = std::chrono::steady_clock::now();
            retained_commit_us = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    commit_finished - commit_started)
                    .count());
        }
        if (retained_graph_committed) {
            const auto committed_at = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(mutex_);
            RecordInteractionCommitLatencyLocked(committed_at);
            retained_projection_dirty_ = false;
            retained_source_content_dirty_ = false;
            retained_flutter_content_dirty_ = false;
            retained_deferred_content_deadline_ = {};
            if (retained_flutter_baked) {
                append_retained_graph_sample(
                    retained_graph_flutter_bake_us_,
                    retained_flutter_bake_us);
            }
            if (retained_source_baked) {
                append_retained_graph_sample(
                    retained_graph_source_bake_us_,
                    retained_source_bake_us);
            }
            append_retained_graph_sample(
                retained_graph_apply_us_, retained_apply_us);
            append_retained_graph_sample(
                retained_graph_commit_us_, retained_commit_us);
            high_refresh_metrics_.record_composite_us(
                static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        committed_at - composite_started)
                        .count()));
            high_refresh_metrics_.record_draw_us(
                static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        retained_draw_finished - acquire_finished)
                        .count()));
            high_refresh_metrics_.record_present_block_us(0);
            high_refresh_metrics_.record_source_projection_reuse();
            if (overlay_layer_state_.snapshot().active) {
                overlay_layer_state_.reuse();
                overlay_layer_state_.composite();
                high_refresh_metrics_.record_overlay_layer_reuse();
                high_refresh_metrics_.record_overlay_composite_us(0);
            }
            diagnostics_.flutter_generation = held_flutter_.frame_generation;
            diagnostics_.video_generation = held_source_.frame_generation;
            diagnostics_.source_projection_enabled = true;
            diagnostics_.source_cache_active = true;
            diagnostics_.source_cache_consumed_generation =
                held_source_.frame_generation;
            diagnostics_.retained_graph_active = true;
            diagnostics_.retained_graph_mode = "content-update";
            diagnostics_.retained_graph_fallback_reason = "none";
            diagnostics_.retained_graph_commit_count =
                retained_graph_commit_count_;
            diagnostics_.retained_graph_projection_commit_count =
                retained_graph_projection_commit_count_;
            diagnostics_.retained_graph_source_bake_count =
                retained_graph_source_bake_count_;
            diagnostics_.retained_graph_flutter_bake_count =
                retained_graph_flutter_bake_count_;
            diagnostics_.retained_graph_projection_skip_present_count =
                retained_graph_projection_skip_present_count_;
            diagnostics_.retained_graph_deferred_content_count =
                retained_graph_deferred_content_count_;
            diagnostics_.retained_graph_commit_defer_count =
                retained_graph_commit_defer_count_;
            if (source_bundle_active) {
                source_cache_error_ = "none";
                diagnostics_.source_cache_last_error = "none";
            }
            return true;
        }
    }
    if (pending_swap_chain_.swap_chain) {
        uint64_t min_video_generation = 0;
        uint64_t min_source_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            min_video_generation = transition_min_video_generation_;
            min_source_generation = transition_min_source_generation_;
        }
        const uint64_t held_video_generation =
            composite_target == OutputTarget::ScRGB
                ? held_video_.frame_generation
                : held_sdr_video_.buffer_generation;
        if (held_video_generation < min_video_generation ||
            (source_projection.enabled &&
             held_source_.frame_generation < min_source_generation)) {
            return false;
        }
    }
    bool ok = true;
    if (ok) {
        D3D11_TEXTURE2D_DESC back_desc = {};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
        SwapChainResources* output =
            pending_swap_chain_.swap_chain
                ? &pending_swap_chain_
                : &current_swap_chain_;
        if (!output->swap_chain ||
            FAILED(output->swap_chain->GetBuffer(
                0, IID_PPV_ARGS(&back_buffer))) ||
            !back_buffer) {
            ok = false;
        }
        if (!ok) {
            return false;
        }
        back_buffer->GetDesc(&back_desc);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto video_width = static_cast<uint32_t>(std::max(
                0,
                composite_target == OutputTarget::ScRGB
                    ? held_video_.width
                    : held_sdr_video_.width));
            const auto video_height = static_cast<uint32_t>(std::max(
                0,
                composite_target == OutputTarget::ScRGB
                    ? held_video_.height
                    : held_sdr_video_.height));
            const bool size_changed =
                back_desc.Width != last_logged_backbuffer_width_ ||
                back_desc.Height != last_logged_backbuffer_height_ ||
                held_flutter_.width != last_logged_flutter_width_ ||
                held_flutter_.height != last_logged_flutter_height_ ||
                video_width != last_logged_video_width_ ||
                video_height != last_logged_video_height_;
            if (size_changed) {
                last_logged_backbuffer_width_ = back_desc.Width;
                last_logged_backbuffer_height_ = back_desc.Height;
                last_logged_flutter_width_ = held_flutter_.width;
                last_logged_flutter_height_ = held_flutter_.height;
                last_logged_video_width_ = video_width;
                last_logged_video_height_ = video_height;
                spdlog::debug(
                    "[WindowsCompositorDebug] dcomp composite dimensions "
                    "swapchain={}x{} flutter={}x{} video={}x{} "
                    "videoGen={} flutterGen={}",
                    back_desc.Width, back_desc.Height,
                    held_flutter_.width, held_flutter_.height,
                    video_width, video_height,
                    composite_target == OutputTarget::ScRGB
                        ? held_video_.frame_generation
                        : held_sdr_video_.buffer_generation,
                    held_flutter_.frame_generation);
            }
        }
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(back_desc.Width);
        viewport.Height = static_cast<float>(back_desc.Height);
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        ID3D11RenderTargetView* rtv = output->rtv.Get();
        if (!rtv) {
            return false;
        }
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = {
            held_video_srv_.Get(),
            held_flutter_srv_.Get(),
            held_source_srvs_[0].Get(),
            held_source_srvs_[1].Get(),
            held_source_srvs_[2].Get(),
            held_source_srvs_[3].Get(),
            held_sdr_video_srv_.Get(),
        };
        context_->PSSetShaderResources(0, 7, srvs);
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        CompositeConstants values = {};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int i = 0; i < 4; ++i) {
                values.viewport[i] = static_cast<float>(viewport_[i]);
                values.background_color[i] = viewport_background_[i];
            }
        }
        values.sdr_white_scale = static_cast<float>(
            sdr_white_scale_.load(std::memory_order_relaxed));
        values.output_mode =
            output->target == OutputTarget::ScRGB ? 1.0f : 0.0f;
        values.source_projection_enabled =
            source_bundle_active ? 1.0f : 0.0f;
        values.source_mode = static_cast<float>(source_projection.mode);
        values.source_split_pos = source_projection.split_pos;
        values.source_track_count =
            static_cast<float>(source_projection.active_track_count);
        for (size_t i = 0; i < 4; ++i) {
            values.source_present[i] =
                held_source_present_[i] ? 1.0f : 0.0f;
            values.source_order[i] =
                static_cast<float>(source_projection.source_order[i]);
            values.source_transfer[i] =
                static_cast<float>(held_source_transfer_[i]);
            values.source_display_offset_x[i] =
                source_projection.display_offset_x[i];
            values.source_display_offset_y[i] =
                source_projection.display_offset_y[i];
            values.source_inv_display_size_x[i] =
                source_projection.inv_display_size_x[i];
            values.source_inv_display_size_y[i] =
                source_projection.inv_display_size_y[i];
            values.source_view_offset_uv_x[i] =
                source_projection.view_offset_uv_x[i];
            values.source_view_offset_uv_y[i] =
                source_projection.view_offset_uv_y[i];
        }
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
        ID3D11Buffer* constants = constants_.Get();
        context_->PSSetConstantBuffers(0, 1, &constants);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context_->PSSetShader(video_pixel_shader_.Get(), nullptr, 0);
        context_->Draw(4, 0);
        if (source_bundle_active && held_source_.overlay) {
            (void)DrawOverlay(
                held_source_.overlay, source_projection, back_desc);
            context_->IASetInputLayout(nullptr);
            context_->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        }
        context_->OMSetBlendState(
            premultiplied_blend_state_.Get(), nullptr, 0xffffffff);
        context_->PSSetShader(flutter_pixel_shader_.Get(), nullptr, 0);
        context_->Draw(4, 0);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        std::array<ID3D11ShaderResourceView*, 7> null_srvs = {};
        context_->PSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
        context_->Flush();
        bool capture = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            capture = diagnostic_capture_pending_;
            diagnostic_capture_pending_ = false;
        }
        if (capture && !CaptureDiagnostics(
                           back_buffer.Get(),
                           held_flutter_texture_.Get())) {
            std::lock_guard<std::mutex> lock(mutex_);
            diagnostic_capture_pending_ = true;
        }
        if (retained_graph_active_ || retained_root_visual_) {
            HRESULT root_result = dcomp_target_
                ? dcomp_target_->SetRoot(dcomp_visual_.Get())
                : E_FAIL;
            if (SUCCEEDED(root_result) && dcomp_device_) {
                root_result = dcomp_device_->Commit();
            }
            if (FAILED(root_result)) {
                retained_graph_fallback_reason_ = "restore-swapchain-root-failed";
                ok = false;
            } else {
                ResetRetainedGraph("full-composite-fallback");
            }
        }
        const auto present_started = std::chrono::steady_clock::now();
        const UINT sync_interval =
            source_bundle_active && source_projection.enabled ? 0u : 1u;
        const HRESULT present_result =
            ok ? output->swap_chain->Present(sync_interval, 0) : E_FAIL;
        const auto present_finished = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            high_refresh_metrics_.record_draw_us(
                static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        present_started - composite_started)
                        .count()));
            high_refresh_metrics_.record_present_block_us(
                static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        present_finished - present_started)
                        .count()));
        }
        ok = SUCCEEDED(present_result);
        if (!ok) {
            const HRESULT removed_reason = device_->GetDeviceRemovedReason();
            if (removed_reason != S_OK) {
                BeginDeviceRecovery(
                    "dcomp-device-removed",
                    static_cast<long>(removed_reason));
                return false;
            }
        }
    }

    if (!ok) {
        Phase phase;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            phase = phase_;
        }
        if (phase != Phase::Failed) {
            EnterFailed("dcomp-composite-or-present-failed");
        }
        return false;
    }
    if (pending_swap_chain_.swap_chain &&
        !ActivatePendingSwapChain()) {
        EnterFailed("dcomp-swap-chain-activation-failed");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto presented_at = std::chrono::steady_clock::now();
        if (last_present_time_.time_since_epoch().count() > 0) {
            high_refresh_metrics_.record_present_interval_us(
                static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        presented_at - last_present_time_)
                        .count()));
        } else {
            high_refresh_metrics_.record_present_interval_us(0);
        }
        last_present_time_ = presented_at;
        high_refresh_metrics_.record_composite_us(
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    presented_at - composite_started)
                    .count()));
        high_refresh_metrics_.record_acquire_wait_us(
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    acquire_finished - composite_started)
                    .count()));
        RecordInteractionCommitLatencyLocked(presented_at);
        if (source_bundle_active) {
            high_refresh_metrics_.record_source_projection_reuse();
        }
        diagnostics_.flutter_generation =
            held_flutter_.frame_generation;
        diagnostics_.video_generation =
            composite_target == OutputTarget::ScRGB
                ? held_video_.frame_generation
                : held_sdr_video_.buffer_generation;
        diagnostics_.source_projection_enabled =
            source_projection.enabled;
        diagnostics_.source_cache_active =
            source_bundle_active &&
            (phase_ == Phase::Preparing || phase_ == Phase::Active);
        diagnostics_.retained_graph_active = retained_graph_active_;
        diagnostics_.retained_graph_mode =
            retained_graph_active_
                ? "active"
                : (source_bundle_active ? "available" : "inactive");
        diagnostics_.retained_graph_fallback_reason =
            retained_graph_fallback_reason_;
        diagnostics_.retained_graph_commit_count =
            retained_graph_commit_count_;
        diagnostics_.retained_graph_projection_commit_count =
            retained_graph_projection_commit_count_;
        diagnostics_.retained_graph_source_bake_count =
            retained_graph_source_bake_count_;
        diagnostics_.retained_graph_flutter_bake_count =
            retained_graph_flutter_bake_count_;
        diagnostics_.retained_graph_projection_skip_present_count =
            retained_graph_projection_skip_present_count_;
        diagnostics_.retained_graph_deferred_content_count =
            retained_graph_deferred_content_count_;
        diagnostics_.retained_graph_commit_defer_count =
            retained_graph_commit_defer_count_;
        if (source_bundle_active) {
            diagnostics_.source_cache_consumed_generation =
                held_source_.frame_generation;
            source_cache_error_ = "none";
            diagnostics_.source_cache_last_error = "none";
            if (!source_cache_consumed_logged_) {
                source_cache_consumed_logged_ = true;
                spdlog::info(
                    "[WindowsNativeCompositor] first source cache bundle "
                    "consumed generation={}",
                    held_source_.frame_generation);
            }
        }
        const bool transition_inputs_ready =
            diagnostics_.video_generation >= transition_min_video_generation_ &&
            (!source_projection.enabled ||
             (source_bundle_active &&
              held_source_.frame_generation >=
                  transition_min_source_generation_));
        if (!pending_swap_chain_.swap_chain &&
            diagnostics_.transition_state == "preparing" &&
            transition_inputs_ready) {
            diagnostics_.transition_state = "stable";
            ++diagnostics_.output_generation;
            if (diagnostics_.device_recovery_state ==
                    vr::windows_device_recovery_state_name(
                        vr::WindowsDeviceRecoveryState::WaitingForFreshVideo) ||
                diagnostics_.device_recovery_state ==
                    vr::windows_device_recovery_state_name(
                        vr::WindowsDeviceRecoveryState::ReactivatingCompositor)) {
                diagnostics_.device_recovery_state =
                    vr::windows_device_recovery_state_name(
                        vr::WindowsDeviceRecoveryState::Recovered);
                ++diagnostics_.device_recovery_success_count;
            }
        }
        ++diagnostics_.composite_count;
        ++diagnostics_.present_count;
    }
    Phase phase;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase = phase_;
    }
    if (phase == Phase::Inactive) {
        engine_api_.set_mode(flutter_view_, kExportCompositorOwned);
        PublishState(Phase::Preparing, "first-dcomp-present");
    }
    return true;
}

bool WindowsNativeCompositor::DrawOverlay(
    const std::shared_ptr<const vr::AnalysisOverlayPrimitivePackage>& overlay,
    const SourceProjection& projection,
    const D3D11_TEXTURE2D_DESC& back_desc) {
    const auto overlay_started = std::chrono::steady_clock::now();
    if (!overlay || overlay->empty() || back_desc.Width == 0 ||
        back_desc.Height == 0) {
        return true;
    }
    vr::WindowsOverlayLayerSignature signature;
    signature.primitive_generation = overlay->cache_generation;
    signature.target_class =
        back_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1u : 0u;
    signature.sdr_white_scale_x1000 = static_cast<uint32_t>(
        std::llround(
            sdr_white_scale_.load(std::memory_order_relaxed) * 1000.0));
    uint64_t track_signature = 1469598103934665603ull;
    const auto mix = [&](uint64_t value) {
        track_signature ^= value + 0x9e3779b97f4a7c15ull +
                           (track_signature << 6) +
                           (track_signature >> 2);
    };
    for (const auto& track : overlay->tracks) {
        mix(static_cast<uint64_t>(track.slot + 17));
        mix(static_cast<uint64_t>(track.track_file_id + 31));
        mix(static_cast<uint64_t>(track.frame_index + 43));
        mix(static_cast<uint64_t>(track.video_width));
        mix(static_cast<uint64_t>(track.video_height));
        mix(static_cast<uint64_t>(track.mode + 59));
        mix(static_cast<uint64_t>(track.opacity_permille + 71));
        mix(track.show_grid ? 1ull : 0ull);
        mix(track.show_qp ? 1ull : 0ull);
        mix(track.show_pred ? 1ull : 0ull);
        mix(track.show_lines ? 1ull : 0ull);
        mix(track.show_bit_cost ? 1ull : 0ull);
        signature.source_width = std::max(
            signature.source_width,
            static_cast<uint32_t>(std::max(track.video_width, 0)));
        signature.source_height = std::max(
            signature.source_height,
            static_cast<uint32_t>(std::max(track.video_height, 0)));
        signature.fill_rect_count += static_cast<uint32_t>(
            std::min<size_t>(track.fill_rects.size(), UINT32_MAX));
        signature.outline_rect_count += static_cast<uint32_t>(
            std::min<size_t>(track.outline_rects.size(), UINT32_MAX));
        signature.motion_line_count += static_cast<uint32_t>(
            std::min<size_t>(track.motion_lines.size(), UINT32_MAX));
    }
    signature.track_signature = track_signature;

    const auto make_color = [&](vr::analysis::OverlayColor color) {
        OverlayVertex vertex;
        const bool scrgb =
            back_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const auto channel = [&](uint8_t value) {
            const float encoded = static_cast<float>(value) / 255.0f;
            return scrgb
                ? srgb_to_linear(encoded) *
                      static_cast<float>(
                          sdr_white_scale_.load(
                              std::memory_order_relaxed))
                : encoded;
        };
        vertex.r = channel(color.r);
        vertex.g = channel(color.g);
        vertex.b = channel(color.b);
        vertex.a = static_cast<float>(color.a) / 255.0f;
        vertex.r *= vertex.a;
        vertex.g *= vertex.a;
        vertex.b *= vertex.a;
        return vertex;
    };

    const bool retained_layer_reusable =
        overlay_vertex_buffer_ &&
        overlay_vertex_count_ > 0 &&
        overlay_layer_signature_ == signature;
    if (retained_layer_reusable) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            overlay_layer_state_.prepare(
                signature,
                static_cast<uint64_t>(overlay_vertex_count_) *
                    sizeof(OverlayVertex));
            high_refresh_metrics_.record_overlay_layer_reuse();
        }
    } else {
        const auto raster_started = std::chrono::steady_clock::now();
        std::vector<OverlayVertex> vertices;
        const auto append_quad = [&](int source_slot,
                                     float left,
                                     float top,
                                     float right,
                                     float bottom,
                                     vr::analysis::OverlayColor color) {
            left = std::clamp(left, 0.0f, 1.0f);
            right = std::clamp(right, 0.0f, 1.0f);
            top = std::clamp(top, 0.0f, 1.0f);
            bottom = std::clamp(bottom, 0.0f, 1.0f);
            if (right <= left || bottom <= top || color.a == 0 ||
                source_slot < 0 || source_slot >= 4) {
                return;
            }
            const auto base = make_color(color);
            const auto vertex = [&](float u, float v) {
                OverlayVertex out = base;
                out.u = u;
                out.v = v;
                out.source_slot = static_cast<float>(source_slot);
                return out;
            };
            const auto p0 = vertex(left, top);
            const auto p1 = vertex(right, top);
            const auto p2 = vertex(left, bottom);
            const auto p3 = vertex(right, bottom);
            vertices.insert(vertices.end(), {p0, p2, p1, p1, p2, p3});
        };
        const auto append_rect =
            [&](int source_slot,
                int video_width,
                int video_height,
                const vr::AnalysisOverlayRectPrimitive& rect,
                bool outline) {
                if (video_width <= 0 || video_height <= 0) {
                    return;
                }
                const float x0 = static_cast<float>(rect.x0) / video_width;
                const float y0 = static_cast<float>(rect.y0) / video_height;
                const float x1 = static_cast<float>(rect.x1) / video_width;
                const float y1 = static_cast<float>(rect.y1) / video_height;
                if (!outline) {
                    append_quad(source_slot, x0, y0, x1, y1, rect.color);
                    return;
                }
                const float px = 1.0f / std::max(video_width, 1);
                const float py = 1.0f / std::max(video_height, 1);
                append_quad(source_slot, x0, y0, x1, y0 + py, rect.color);
                append_quad(source_slot, x0, y1 - py, x1, y1, rect.color);
                append_quad(source_slot, x0, y0, x0 + px, y1, rect.color);
                append_quad(source_slot, x1 - px, y0, x1, y1, rect.color);
            };
        const auto append_line =
            [&](int source_slot,
                int video_width,
                int video_height,
                const vr::AnalysisOverlayLinePrimitive& line) {
                if (video_width <= 0 || video_height <= 0 ||
                    source_slot < 0 || source_slot >= 4 ||
                    line.color.a == 0) {
                    return;
                }
                const float x0 =
                    static_cast<float>(line.x0) / video_width;
                const float y0 =
                    static_cast<float>(line.y0) / video_height;
                const float x1 =
                    static_cast<float>(line.x1) / video_width;
                const float y1 =
                    static_cast<float>(line.y1) / video_height;
                const float dx = x1 - x0;
                const float dy = y1 - y0;
                const float length =
                    std::max(std::sqrt(dx * dx + dy * dy), 0.0001f);
                const float nx =
                    -dy / length * (0.5f / std::max(video_width, 1));
                const float ny =
                    dx / length * (0.5f / std::max(video_height, 1));
                const auto base = make_color(line.color);
                const auto vertex = [&](float u, float v) {
                    OverlayVertex out = base;
                    out.u = u;
                    out.v = v;
                    out.source_slot = static_cast<float>(source_slot);
                    return out;
                };
                const auto p0 = vertex(x0 + nx, y0 + ny);
                const auto p1 = vertex(x0 - nx, y0 - ny);
                const auto p2 = vertex(x1 + nx, y1 + ny);
                const auto p3 = vertex(x1 - nx, y1 - ny);
                vertices.insert(vertices.end(), {p0, p1, p2, p2, p1, p3});
            };

        for (const auto& track : overlay->tracks) {
            for (const auto& rect : track.fill_rects) {
                append_rect(
                    track.slot,
                    track.video_width,
                    track.video_height,
                    rect,
                    false);
            }
            for (const auto& rect : track.outline_rects) {
                append_rect(
                    track.slot,
                    track.video_width,
                    track.video_height,
                    rect,
                    true);
            }
            for (const auto& line : track.motion_lines) {
                append_line(
                    track.slot,
                    track.video_width,
                    track.video_height,
                    line);
            }
        }
        const auto raster_finished = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            high_refresh_metrics_.record_overlay_raster_us(
                static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        raster_finished - raster_started)
                        .count()));
        }
        if (vertices.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            overlay_layer_state_.miss("overlay-layer-empty");
            return true;
        }

        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = static_cast<UINT>(
            vertices.size() * sizeof(OverlayVertex));
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = vertices.data();
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        const auto upload_started = std::chrono::steady_clock::now();
        if (FAILED(device_->CreateBuffer(&desc, &data, &buffer))) {
            overlay_vertex_buffer_.Reset();
            overlay_vertex_count_ = 0;
            overlay_layer_signature_ = {};
            std::lock_guard<std::mutex> lock(mutex_);
            overlay_layer_state_.fail("overlay-layer-buffer-create-failed");
            return true;
        }
        const auto upload_finished = std::chrono::steady_clock::now();
        overlay_vertex_buffer_ = std::move(buffer);
        overlay_vertex_count_ = static_cast<UINT>(vertices.size());
        overlay_layer_signature_ = signature;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            overlay_layer_state_.prepare(
                signature,
                static_cast<uint64_t>(desc.ByteWidth));
            high_refresh_metrics_.record_overlay_layer_raster();
            high_refresh_metrics_.record_overlay_layer_upload();
            high_refresh_metrics_.record_overlay_upload_us(
                static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        upload_finished - upload_started)
                        .count()));
        }
    }
    if (!overlay_vertex_buffer_ || overlay_vertex_count_ == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        overlay_layer_state_.miss("overlay-layer-buffer-missing");
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.overlay_generation = overlay->cache_generation;
        diagnostics_.overlay_fill_rect_count = 0;
        diagnostics_.overlay_line_rect_count = 0;
        diagnostics_.overlay_motion_line_count = 0;
        for (const auto& track : overlay->tracks) {
            diagnostics_.overlay_fill_rect_count += track.fill_rects.size();
            diagnostics_.overlay_line_rect_count += track.outline_rects.size();
            diagnostics_.overlay_motion_line_count +=
                track.motion_lines.size();
        }
    }
    UINT stride = sizeof(OverlayVertex);
    UINT offset = 0;
    ID3D11Buffer* raw_buffer = overlay_vertex_buffer_.Get();
    context_->IASetVertexBuffers(0, 1, &raw_buffer, &stride, &offset);
    context_->IASetInputLayout(overlay_input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(overlay_vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(overlay_pixel_shader_.Get(), nullptr, 0);
    context_->OMSetBlendState(
        overlay_blend_state_.Get(), nullptr, 0xffffffff);
    context_->Draw(overlay_vertex_count_, 0);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        overlay_layer_state_.composite();
        const auto overlay_state = overlay_layer_state_.snapshot();
        diagnostics_.overlay_retained_layer_active = overlay_state.active;
        diagnostics_.overlay_layer_mode = overlay_state.mode;
        diagnostics_.overlay_layer_texture_count =
            overlay_state.texture_count;
        diagnostics_.overlay_layer_bytes = overlay_state.bytes;
        diagnostics_.overlay_layer_generation = overlay_state.generation;
        diagnostics_.overlay_layer_committed_generation =
            overlay_state.committed_generation;
        diagnostics_.overlay_layer_pending_generation =
            overlay_state.pending_generation;
        diagnostics_.overlay_layer_composite_count =
            overlay_state.composite_count;
        diagnostics_.overlay_layer_miss_count = overlay_state.miss_count;
        diagnostics_.overlay_layer_backpressure_count =
            overlay_state.backpressure_count;
        diagnostics_.overlay_layer_fallback_reason =
            overlay_state.fallback_reason;
        diagnostics_.overlay_layer_last_error = overlay_state.last_error;
        high_refresh_metrics_.record_overlay_composite_us(
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - overlay_started)
                    .count()));
    }
    return true;
}

bool WindowsNativeCompositor::CaptureDiagnostics(
    ID3D11Texture2D* back_buffer,
    ID3D11Texture2D* flutter_texture) {
    if (!back_buffer || !flutter_texture) {
        return false;
    }
    D3D11_TEXTURE2D_DESC final_desc = {};
    D3D11_TEXTURE2D_DESC flutter_desc = {};
    back_buffer->GetDesc(&final_desc);
    flutter_texture->GetDesc(&flutter_desc);
    final_desc.Usage = D3D11_USAGE_STAGING;
    final_desc.BindFlags = 0;
    final_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    final_desc.MiscFlags = 0;
    flutter_desc.Usage = D3D11_USAGE_STAGING;
    flutter_desc.BindFlags = 0;
    flutter_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    flutter_desc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> final_staging;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> flutter_staging;
    if (FAILED(device_->CreateTexture2D(
            &final_desc, nullptr, &final_staging)) ||
        FAILED(device_->CreateTexture2D(
            &flutter_desc, nullptr, &flutter_staging))) {
        return false;
    }
    context_->CopyResource(final_staging.Get(), back_buffer);
    context_->CopyResource(flutter_staging.Get(), flutter_texture);

    D3D11_MAPPED_SUBRESOURCE final_map = {};
    D3D11_MAPPED_SUBRESOURCE flutter_map = {};
    if (FAILED(context_->Map(
            final_staging.Get(), 0, D3D11_MAP_READ, 0, &final_map))) {
        return false;
    }
    if (FAILED(context_->Map(
            flutter_staging.Get(), 0, D3D11_MAP_READ, 0, &flutter_map))) {
        context_->Unmap(final_staging.Get(), 0);
        return false;
    }

    double alpha_sum = 0.0;
    uint64_t transparent_pixels = 0;
    uint64_t pixels_over_1 = 0;
    float max_rgb = 0.0f;
    const uint64_t flutter_pixels =
        static_cast<uint64_t>(flutter_desc.Width) * flutter_desc.Height;
    for (UINT y = 0; y < flutter_desc.Height; ++y) {
        const auto* row = static_cast<const uint8_t*>(flutter_map.pData) +
                          static_cast<size_t>(y) * flutter_map.RowPitch;
        for (UINT x = 0; x < flutter_desc.Width; ++x) {
            const uint8_t alpha = row[x * 4u + 3u];
            alpha_sum += static_cast<double>(alpha) / 255.0;
            if (alpha == 0) {
                ++transparent_pixels;
            }
        }
    }
    const uint64_t final_pixels =
        static_cast<uint64_t>(final_desc.Width) * final_desc.Height;
    for (UINT y = 0; y < final_desc.Height; ++y) {
        const auto* bytes =
            static_cast<const uint8_t*>(final_map.pData) +
            static_cast<size_t>(y) * final_map.RowPitch;
        for (UINT x = 0; x < final_desc.Width; ++x) {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            if (final_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const auto* row =
                    reinterpret_cast<const uint16_t*>(bytes);
                r = vr::half_to_float(row[x * 4u]);
                g = vr::half_to_float(row[x * 4u + 1u]);
                b = vr::half_to_float(row[x * 4u + 2u]);
            } else {
                b = static_cast<float>(bytes[x * 4u]) / 255.0f;
                g = static_cast<float>(bytes[x * 4u + 1u]) / 255.0f;
                r = static_cast<float>(bytes[x * 4u + 2u]) / 255.0f;
            }
            const float pixel_max = std::max({r, g, b});
            max_rgb = std::max(max_rgb, pixel_max);
            if (pixel_max > 1.0f) {
                ++pixels_over_1;
            }
        }
    }
    context_->Unmap(flutter_staging.Get(), 0);
    context_->Unmap(final_staging.Get(), 0);

    std::lock_guard<std::mutex> lock(mutex_);
    diagnostics_.flutter_alpha_average_x1000 =
        flutter_pixels == 0
            ? 0
            : static_cast<uint64_t>(
                  std::llround(alpha_sum * 1000.0 / flutter_pixels));
    diagnostics_.flutter_transparent_pixels_x1000 =
        flutter_pixels == 0
            ? 0
            : transparent_pixels * 1000u / flutter_pixels;
    diagnostics_.final_max_rgb_x1000 =
        static_cast<uint64_t>(
            std::llround(std::max(max_rgb, 0.0f) * 1000.0f));
    diagnostics_.final_pixels_over_1 = pixels_over_1;
    ++diagnostics_.diagnostic_capture_count;
    return final_pixels != 0;
}

void WindowsNativeCompositor::SignalWork() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        work_pending_ = true;
    }
    wake_.notify_one();
}

void WindowsNativeCompositor::EnterFailed(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::Failed) {
            return;
        }
        ++diagnostics_.failure_count;
        diagnostics_.fallback_reason = reason;
        terminal_inactive_ = true;
        diagnostics_.swap_chain_active =
            diagnostics_.present_count > 0 && current_swap_chain_.swap_chain;
    }
    if (auto player = player_.lock()) {
        player->clear_source_cache(reason.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_projection_ = {};
        diagnostics_.source_projection_enabled = false;
        diagnostics_.source_cache_active = false;
        source_cache_error_ =
            reason.empty() ? "native-compositor-failed" : reason;
        diagnostics_.source_cache_last_error = source_cache_error_;
    }
    PublishState(Phase::Failed, reason);
}

void WindowsNativeCompositor::PublishState(
    Phase phase, const std::string& reason) {
    StateCallback callback;
    uint64_t serial = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = phase;
        diagnostics_.phase = PhaseName(phase);
        if (phase != Phase::Inactive) {
            serial = ++state_serial_;
        } else {
            serial = state_serial_;
        }
        diagnostics_.state_serial = serial;
        if (phase == Phase::Failed) {
            diagnostics_.fallback_reason = reason;
        }
        callback = state_callback_;
    }
    if (callback) callback(phase, serial, reason);
    spdlog::info(
        "[WindowsNativeCompositor] phase={} serial={} reason={}",
        PhaseName(phase), serial, reason);
}

const char* WindowsNativeCompositor::PhaseName(Phase phase) {
    switch (phase) {
    case Phase::Inactive: return "inactive";
    case Phase::Preparing: return "preparing";
    case Phase::Active: return "active";
    case Phase::Failed: return "failed";
    }
    return "inactive";
}

const char* WindowsNativeCompositor::OutputTargetName(
    OutputTarget target) {
    return target == OutputTarget::ScRGB
        ? "scrgb"
        : "sdr";
}

const char* WindowsNativeCompositor::OutputFormatName(
    OutputTarget target) {
    return target == OutputTarget::ScRGB
        ? "R16G16B16A16_FLOAT"
        : "B8G8R8A8_UNORM";
}

const char* WindowsNativeCompositor::OutputColorSpaceName(
    OutputTarget target) {
    return target == OutputTarget::ScRGB
        ? "RGB_FULL_G10_NONE_P709"
        : "RGB_FULL_G22_NONE_P709";
}
