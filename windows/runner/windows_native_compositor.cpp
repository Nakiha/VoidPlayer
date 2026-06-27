#include "windows_native_compositor.h"

#include "windows/presentation/windows_dcomp_composite.h"

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace {

constexpr int kExportDisabled = 0;
constexpr int kExportMirror = 1;
constexpr int kExportCompositorOwned = 2;
constexpr auto kFlutterExportStaleTimeout = std::chrono::milliseconds(750);
constexpr int64_t kMinUnrequestedFlutterExportSignalUs = 1000;
constexpr int64_t kMaxUnrequestedFlutterExportSignalUs = 16667;
constexpr auto kFlutterExportPacingSampleInterval =
    std::chrono::milliseconds(250);

struct CompositeConstants {
    float viewport[4];
    float background_color[4];
    float sdr_white_scale;
    float output_mode;
    float sdr_video_is_scrgb;
    float present_padding;
};

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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
        work_pending_ = true;
        terminal_inactive_ = false;
        rate_start_time_ = std::chrono::steady_clock::now();
        source_cache_publish_count_ = 0;
        flutter_export_unsolicited_signal_count_ = 0;
        flutter_export_unsolicited_throttle_count_ = 0;
        last_unsolicited_flutter_export_signal_ = {};
        last_explicit_flutter_frame_request_time_ = {};
        high_refresh_metrics_.reset(diagnostics_.high_refresh_display_hz);
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
    (void)RequestFlutterFrame("startup-bootstrap");
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
        player->clear_source_cache(reason ? reason : "compositor-stop");
    }
    if (engine_api_.available() && flutter_view_) {
        engine_api_.set_callback(flutter_view_, nullptr, nullptr);
        engine_api_.set_mode(flutter_view_, kExportDisabled);
    }
    if (dcomp_target_) dcomp_target_->SetRoot(nullptr);
    if (dcomp_device_) dcomp_device_->Commit();
    if (d3d12_present_target_) {
        d3d12_present_target_->shutdown();
        d3d12_present_target_.reset();
    }
    pending_swap_chain_ = {};
    current_swap_chain_ = {};
    retained_graph_fallback_reason_ = "stop";
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();
    constants_.Reset();
    sampler_.Reset();
    video_pixel_shader_.Reset();
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
    if (held_flutter_valid_) {
        if (engine_api_.available() && flutter_view_ &&
            held_flutter_.lease_id != 0) {
            engine_api_.release(flutter_view_, held_flutter_.lease_id);
        }
    }
    held_flutter_valid_ = false;
    held_flutter_ = {};
    held_flutter_d3d12_resource_.Reset();
    external_flutter_surface_submitted_generation_ = 0;
    external_flutter_surface_refresh_generation_ = 0;
    if (player) {
        player->clear_external_flutter_surface();
    }

    if (held_video_valid_) {
        if (held_video_mutex_ &&
            held_video_.sync_mode ==
                vr::SharedFp16TextureSyncMode::KeyedMutex) {
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
        if (terminal_inactive_ || phase == Phase::Failed) {
            return false;
        }
        request_sequence = ++flutter_frame_request_sequence_;
        base_generation = pending_flutter_frame_request_base_generation_;
        const bool bootstrap =
            phase == Phase::Inactive || phase == Phase::Preparing;
        track_surface_update = !bootstrap;
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
    const bool bootstrap =
        phase == Phase::Inactive || phase == Phase::Preparing;
    bool ok = false;
    bool request_ok = false;
    if (bootstrap) {
        ok = engine_api_.set_mode &&
             engine_api_.set_mode(flutter_view_, kExportMirror);
        request_ok = engine_api_.request_frame &&
                     engine_api_.request_frame(flutter_view_);
        ok = ok && request_ok;
    } else {
        ok = engine_api_.request_frame &&
             engine_api_.request_frame(flutter_view_);
        request_ok = ok;
    }
    spdlog::debug(
        "[WindowsCompositorDebug] request flutter frame reason={} "
        "phase={} action={} seq={} baseGeneration={} ok={} requestOk={}",
        reason.empty() ? "unspecified" : reason,
        PhaseName(phase),
        bootstrap ? "mirror-bootstrap" : "request-compositor-owned-export",
        request_sequence,
        base_generation,
        ok,
        request_ok);
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
    work_pending_ = true;
    wake_.notify_one();
}

void WindowsNativeCompositor::DisableRetainedSourceProjection(
    const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    diagnostics_.source_projection_enabled = false;
    retained_graph_fallback_reason_ =
        reason.empty() ? "backend-source-projection" : reason;
}

void WindowsNativeCompositor::ClearSourceProjection(
    const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    diagnostics_.source_projection_enabled = false;
    diagnostics_.source_cache_active = false;
    source_cache_error_ = reason.empty() ? "clear-requested" : reason;
    diagnostics_.source_cache_last_error = source_cache_error_;
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
    pending_output_adapter_ = output_adapter_ ? output_adapter_ : producer_adapter_;
    work_pending_ = true;
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
    result.overlay_retained_layer_active = false;
    result.overlay_layer_mode = "inactive";
    result.overlay_layer_texture_count = 0;
    result.overlay_layer_bytes = 0;
    result.overlay_layer_generation = 0;
    result.overlay_layer_committed_generation = 0;
    result.overlay_layer_pending_generation = 0;
    result.overlay_layer_composite_count = 0;
    result.overlay_layer_miss_count = 0;
    result.overlay_layer_backpressure_count = 0;
    result.overlay_layer_fallback_reason = "none";
    result.overlay_layer_last_error = "none";
    result.flutter_export_unsolicited_signal_count =
        flutter_export_unsolicited_signal_count_;
    result.flutter_export_unsolicited_throttle_count =
        flutter_export_unsolicited_throttle_count_;
    const bool hot_path_active =
        phase_ == Phase::Active && result.source_projection_enabled;
    const auto gate_result = vr::evaluate_windows_high_refresh_gate(
        high_refresh, hot_path_active, false);
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
    result.retained_graph_active = false;
    result.retained_graph_mode = "inactive";
    result.retained_graph_fallback_reason =
        retained_graph_fallback_reason_;
    result.retained_graph_commit_count = 0;
    result.retained_graph_projection_commit_count = 0;
    result.retained_graph_source_bake_count = 0;
    result.retained_graph_flutter_bake_count = 0;
    result.retained_graph_projection_skip_present_count = 0;
    result.retained_graph_deferred_content_count = 0;
    result.retained_graph_commit_defer_count = 0;
    result.retained_graph_flutter_bake_p95_us = 0;
    result.retained_graph_source_bake_p95_us = 0;
    result.retained_graph_apply_p95_us = 0;
    result.retained_graph_commit_p95_us = 0;
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
        } else if (compositor->phase_ != Phase::Active) {
            signal_work = true;
        } else {
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
    engine_api_.acquire_v2 = reinterpret_cast<AcquireFlutterSurfaceV2Fn>(
        GetProcAddress(module, "FlutterDesktopViewAcquireLatestSurfaceV2"));
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
    if (d3d12_present_target_) {
        d3d12_present_target_->shutdown();
        d3d12_present_target_.reset();
    }
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();
    retained_graph_fallback_reason_ = "device-rebuild";
    constants_.Reset();
    sampler_.Reset();
    video_pixel_shader_.Reset();
    vertex_shader_.Reset();
    context_.Reset();
    device_.Reset();
    video_transport_.reset();
    sdr_video_transport_.reset();

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
            sdr_video_transport_.copy_count();
        diagnostics_.transport_copy_bytes =
            video_transport_.copy_count() * video_transport_.bytes_per_copy() +
            sdr_video_transport_.copy_count() *
                sdr_video_transport_.bytes_per_copy();
        diagnostics_.transport_timeout_count =
            video_transport_.timeout_count() +
            sdr_video_transport_.timeout_count();
        diagnostics_.transport_last_copy_us = transport.last_copy_us();
        diagnostics_.transport_total_copy_us =
            video_transport_.total_copy_us() +
            sdr_video_transport_.total_copy_us();
        diagnostics_.shared_fence_signal_count =
            video_transport_.shared_fence_signal_count() +
            sdr_video_transport_.shared_fence_signal_count();
        diagnostics_.shared_fence_wait_count =
            video_transport_.shared_fence_wait_count() +
            sdr_video_transport_.shared_fence_wait_count();
        diagnostics_.shared_fence_timeout_count =
            video_transport_.shared_fence_timeout_count() +
            sdr_video_transport_.shared_fence_timeout_count();
        diagnostics_.shared_fence_last_wait_us =
            transport.shared_fence_last_wait_us();
        diagnostics_.shared_fence_p95_wait_us =
            std::max({
                video_transport_.shared_fence_p95_wait_us(),
                sdr_video_transport_.shared_fence_p95_wait_us()});
        diagnostics_.event_query_p95_wait_us =
            std::max({
                video_transport_.event_query_p95_wait_us(),
                sdr_video_transport_.event_query_p95_wait_us()});
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
    Microsoft::WRL::ComPtr<ID3DBlob> video_ps_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, &vs_blob, &errors);
    if (FAILED(hr)) return log_compile_failure("compile VSMain", hr, errors);
    errors.Reset();
    hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "PSVideo", "ps_5_0", 0, 0, &video_ps_blob, &errors);
    if (FAILED(hr)) return log_compile_failure("compile PSVideo", hr, errors);
    errors.Reset();
    hr = device_->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
            nullptr, &vertex_shader_);
    if (FAILED(hr)) return log_failure("CreateVertexShader", hr);
    hr = device_->CreatePixelShader(
            video_ps_blob->GetBufferPointer(),
            video_ps_blob->GetBufferSize(),
            nullptr,
            &video_pixel_shader_);
    if (FAILED(hr)) return log_failure("CreatePixelShader PSVideo", hr);
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sampler_desc, &sampler_))) return false;
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
                spdlog::error(
                    "[WindowsNativeCompositor] first present timeout: "
                    "heldVideo={} heldSdrVideo={} heldFlutter={} "
                    "videoGen={} sdrVideoGen={} flutterGen={} "
                    "flutterExportPublish={} flutterExportPresent={} "
                    "flutterExportRequest={} flutterExportDispatch={} "
                    "flutterExportSchedule={} flutterExportVsync={} "
                    "flutterLatest={} flutterPendingPump={}",
                    held_video_valid_,
                    held_sdr_video_valid_,
                    held_flutter_valid_,
                    held_video_.frame_generation,
                    held_sdr_video_.buffer_generation,
                    held_flutter_.frame_generation,
                    diagnostics_.flutter_export_publish_count,
                    diagnostics_.flutter_export_present_count,
                    diagnostics_.flutter_export_request_count,
                    diagnostics_.flutter_export_request_dispatch_count,
                    diagnostics_.flutter_export_schedule_frame_count,
                    diagnostics_.flutter_export_vsync_count,
                    diagnostics_.flutter_export_latest_available,
                    diagnostics_.flutter_export_pending_frame_pump_frames);
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
                    0,
                    0,
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

    const auto release_held_video = [&]() {
        if (!held_video_valid_) return;
        if (held_video_mutex_ &&
            held_video_.sync_mode ==
                vr::SharedFp16TextureSyncMode::KeyedMutex) {
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
        engine_api_.release(flutter_view_, held_flutter_.lease_id);
        held_flutter_valid_ = false;
        held_flutter_ = {};
        held_flutter_d3d12_resource_.Reset();
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
    const auto acquire_legacy_video_inputs = [&]() {
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
                bool acquired = next_video.sync_mode ==
                    vr::SharedFp16TextureSyncMode::PublishedAfterProducerWait;
                const HRESULT open_result = OpenInputTexture(
                    device1.Get(), next_video.handle, &texture)
                    ? S_OK
                    : E_FAIL;
                if (SUCCEEDED(open_result) &&
                    next_video.sync_mode ==
                        vr::SharedFp16TextureSyncMode::KeyedMutex &&
                    SUCCEEDED(texture.As(&keyed_mutex))) {
                    acquired = keyed_mutex->AcquireSync(
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
                    spdlog::debug(
                        "[WindowsNativeCompositor] acquired shared FP16 video "
                        "generation={} ring={} size={}x{} sync={}",
                        held_video_.frame_generation,
                        held_video_.ring_generation,
                        held_video_.width,
                        held_video_.height,
                        static_cast<int>(held_video_.sync_mode));
                    if (IsCrossAdapterActive()) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        diagnostics_.video_transport_generation =
                            diagnostics_.transport_generation;
                    }
                } else {
                    spdlog::debug(
                        "[WindowsNativeCompositor] failed to acquire shared "
                        "FP16 video generation={} ring={} size={}x{} open={} "
                        "acquired={} crossAdapter={}",
                        next_video.frame_generation,
                        next_video.ring_generation,
                        next_video.width,
                        next_video.height,
                        SUCCEEDED(open_result),
                        acquired,
                        IsCrossAdapterActive());
                    if (acquired && keyed_mutex) {
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
    };
    bool legacy_inputs_preacquired = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        legacy_inputs_preacquired = desired_output_target_ != OutputTarget::SDR;
    }
    if (legacy_inputs_preacquired) {
        acquire_legacy_video_inputs();
    }

    FlutterSurface next_flutter;
    Microsoft::WRL::ComPtr<ID3D12Resource> next_flutter_d3d12_resource;
    const auto acquire_flutter_surface = [&]() -> bool {
        const auto assign_v2_surface =
            [&](const FlutterSurfaceV2& surface) {
                next_flutter.backend = surface.backend;
                next_flutter.sync = surface.sync;
                next_flutter.shared_texture_handle = surface.texture_handle;
                next_flutter.fence_handle = surface.fence_handle;
                next_flutter.fence_value = surface.fence_value;
                next_flutter.width = surface.width;
                next_flutter.height = surface.height;
                next_flutter.format = surface.format;
                next_flutter.alpha_mode = surface.alpha_mode;
                next_flutter.ring_generation = surface.ring_generation;
                next_flutter.frame_generation = surface.frame_generation;
                next_flutter.slot = surface.slot;
                next_flutter.consumer_acquire_key =
                    surface.consumer_acquire_key;
                next_flutter.producer_release_key =
                    surface.producer_release_key;
                next_flutter.lease_id = surface.lease_id;
            };
        FlutterSurfaceAcquireOptions d3d12_options;
        d3d12_options.requested_backend = FlutterSurfaceBackend::D3D12;
        FlutterSurfaceV2 d3d12_surface;
        if (!engine_api_.acquire_v2(
                flutter_view_, &d3d12_options, &d3d12_surface)) {
            return false;
        }
        if (d3d12_surface.backend != FlutterSurfaceBackend::D3D12) {
            if (d3d12_surface.lease_id != 0) {
                engine_api_.release(flutter_view_, d3d12_surface.lease_id);
            }
            spdlog::debug(
                "[WindowsCompositorDebug] flutter export rejected "
                "non-D3D12 backend={}",
                static_cast<int>(d3d12_surface.backend));
            return false;
        }
        Microsoft::WRL::ComPtr<ID3D12Device> render_device;
        if (auto* raw_device = static_cast<ID3D12Device*>(
                player->native_render_device())) {
            raw_device->QueryInterface(IID_PPV_ARGS(&render_device));
        }
        HRESULT open_result = E_FAIL;
        if (render_device && d3d12_surface.texture_handle) {
            open_result = render_device->OpenSharedHandle(
                d3d12_surface.texture_handle,
                IID_PPV_ARGS(&next_flutter_d3d12_resource));
        }
        if (SUCCEEDED(open_result) && next_flutter_d3d12_resource) {
            assign_v2_surface(d3d12_surface);
            spdlog::debug(
                "[WindowsCompositorDebug] flutter D3D12 export "
                "opened generation={} ring={} slot={} size={}x{} "
                "lease={} sync={} fenceValue={}",
                d3d12_surface.frame_generation,
                d3d12_surface.ring_generation,
                d3d12_surface.slot,
                d3d12_surface.width,
                d3d12_surface.height,
                d3d12_surface.lease_id,
                static_cast<int>(d3d12_surface.sync),
                d3d12_surface.fence_value);
            return true;
        }
        spdlog::debug(
            "[WindowsCompositorDebug] flutter D3D12 export open failed "
            "hr=0x{:08x}",
            static_cast<uint32_t>(open_result));
        engine_api_.release(flutter_view_, d3d12_surface.lease_id);
        return false;
    };
    if (acquire_flutter_surface()) {
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
            if (next_flutter_d3d12_resource) {
                release_held_flutter();
                held_flutter_ = next_flutter;
                held_flutter_d3d12_resource_ =
                    std::move(next_flutter_d3d12_resource);
                held_flutter_valid_ = true;
                vr::PresentationExternalD3D12Surface external_surface;
                external_surface.resource =
                    held_flutter_d3d12_resource_.Get();
                external_surface.fence_handle = held_flutter_.fence_handle;
                external_surface.width =
                    static_cast<int32_t>(held_flutter_.width);
                external_surface.height =
                    static_cast<int32_t>(held_flutter_.height);
                external_surface.format =
                    static_cast<int32_t>(held_flutter_.format);
                external_surface.sync =
                    static_cast<int32_t>(held_flutter_.sync);
                external_surface.fence_value = held_flutter_.fence_value;
                external_surface.ring_generation =
                    held_flutter_.ring_generation;
                external_surface.frame_generation =
                    held_flutter_.frame_generation;
                const bool submitted_new_external_surface =
                    held_flutter_.frame_generation !=
                    external_flutter_surface_submitted_generation_;
                bool should_refresh_external_surface = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    should_refresh_external_surface =
                        pending_flutter_frame_request_sequence_ != 0 &&
                        held_flutter_.frame_generation >
                            pending_flutter_frame_request_base_generation_;
                }
                const bool external_surface_ready =
                    player->update_external_flutter_surface(external_surface);
                if (external_surface_ready) {
                    external_flutter_surface_submitted_generation_ =
                        held_flutter_.frame_generation;
                }
                if (external_surface_ready &&
                    submitted_new_external_surface &&
                    should_refresh_external_surface &&
                    held_flutter_.frame_generation !=
                        external_flutter_surface_refresh_generation_) {
                    player->request_frame_refresh(
                        "windows-d3d12-flutter-surface-overlay");
                    external_flutter_surface_refresh_generation_ =
                        held_flutter_.frame_generation;
                }
                complete_pending_flutter_request(held_flutter_);
                ++flutter_generation_log_count_;
                if (flutter_generation_log_count_ <= 8 ||
                    flutter_generation_log_count_ % 60 == 0) {
                    spdlog::debug(
                        "[WindowsCompositorDebug] dcomp acquired flutter "
                        "surface generation={} ring={} slot={} size={}x{} "
                        "lease={} backend={} d3d12Resource={}",
                        held_flutter_.frame_generation,
                        held_flutter_.ring_generation,
                        held_flutter_.slot,
                        held_flutter_.width,
                        held_flutter_.height,
                        held_flutter_.lease_id,
                        static_cast<int>(held_flutter_.backend),
                        held_flutter_d3d12_resource_ != nullptr);
                }
            } else {
                log_pending_flutter_request_acquire(
                    "d3d12-resource-open-failed",
                    &next_flutter);
                engine_api_.release(
                    flutter_view_, next_flutter.lease_id);
            }
        }
    } else {
        log_pending_flutter_request_acquire("acquire-latest-failed", nullptr);
    }

    if (!held_flutter_valid_) {
        return false;
    }
    {
        const auto direct_started = std::chrono::steady_clock::now();
        Microsoft::WRL::ComPtr<ID3D12Device> render_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> render_queue;
        if (auto* raw_device =
                static_cast<ID3D12Device*>(player->native_render_device())) {
            raw_device->QueryInterface(IID_PPV_ARGS(&render_device));
        }
        if (auto* raw_queue = static_cast<ID3D12CommandQueue*>(
                player->native_render_command_queue())) {
            raw_queue->QueryInterface(IID_PPV_ARGS(&render_queue));
        }
        OutputTarget direct_target = OutputTarget::SDR;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            direct_target = desired_output_target_;
        }
        if (direct_target != OutputTarget::SDR) {
            if (d3d12_present_target_) {
                d3d12_present_target_->shutdown();
            }
            goto d3d11_compositor_fallback;
        }
        const auto present_format =
            direct_target == OutputTarget::ScRGB
                ? vr::WindowsD3D12PresentTargetFormat::ScRGB
                : vr::WindowsD3D12PresentTargetFormat::SDR;
        if (render_device && render_queue) {
            uint32_t direct_width =
                static_cast<uint32_t>(std::max(1, player->texture_width()));
            uint32_t direct_height =
                static_cast<uint32_t>(std::max(1, player->texture_height()));
            if (direct_width <= 1 || direct_height <= 1) {
                direct_width = held_flutter_.width;
                direct_height = held_flutter_.height;
            }
            if (!d3d12_present_target_) {
                d3d12_present_target_ =
                    std::make_unique<vr::WindowsD3D12PresentTarget>();
            }
            const bool target_matches =
                d3d12_present_target_->active() &&
                d3d12_present_target_->width() == direct_width &&
                d3d12_present_target_->height() == direct_height &&
                ((direct_target == OutputTarget::ScRGB &&
                  d3d12_present_target_->dxgi_format() ==
                      DXGI_FORMAT_R16G16B16A16_FLOAT) ||
                 (direct_target == OutputTarget::SDR &&
                  d3d12_present_target_->dxgi_format() ==
                      DXGI_FORMAT_B8G8R8A8_UNORM));
            if (!target_matches) {
                d3d12_present_target_->shutdown();
                (void)d3d12_present_target_->initialize(
                    hwnd_,
                    render_device.Get(),
                    render_queue.Get(),
                    direct_width,
                    direct_height,
                    present_format);
            }
            vr::WindowsD3D12PresentTargetFrame direct_frame;
            if (d3d12_present_target_->active() &&
                d3d12_present_target_->acquire_frame(direct_frame)) {
                vr::PresentationExternalD3D12RenderTarget render_target;
                render_target.resource = direct_frame.resource.Get();
                render_target.width =
                    static_cast<int32_t>(direct_frame.width);
                render_target.height =
                    static_cast<int32_t>(direct_frame.height);
                render_target.format =
                    static_cast<int32_t>(direct_frame.dxgi_format);
                render_target.color_space =
                    static_cast<int32_t>(direct_frame.color_space);
                if (player->draw_current_frame_to_external_d3d12_target(
                        render_target, "windows-d3d12-direct-present")) {
                    const auto present_started =
                        std::chrono::steady_clock::now();
                    const bool presented =
                        d3d12_present_target_->present(1);
                    const auto present_finished =
                        std::chrono::steady_clock::now();
                    if (presented) {
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            ++d3d12_direct_present_count_;
                            if (d3d12_direct_present_count_ <= 8 ||
                                d3d12_direct_present_count_ % 60 == 0) {
                                spdlog::info(
                                    "[WindowsNativeCompositor] D3D12 direct "
                                    "present count={} target={} size={}x{} "
                                    "flutter={}x{}",
                                    d3d12_direct_present_count_,
                                    OutputTargetName(direct_target),
                                    direct_frame.width,
                                    direct_frame.height,
                                    held_flutter_.width,
                                    held_flutter_.height);
                            }
                            high_refresh_metrics_.record_draw_us(
                                static_cast<int64_t>(
                                    std::chrono::duration_cast<
                                        std::chrono::microseconds>(
                                        present_started - direct_started)
                                        .count()));
                            high_refresh_metrics_.record_present_block_us(
                                static_cast<int64_t>(
                                    std::chrono::duration_cast<
                                        std::chrono::microseconds>(
                                        present_finished - present_started)
                                        .count()));
                            diagnostics_.swap_chain_active = true;
                            diagnostics_.swap_chain_width = direct_frame.width;
                            diagnostics_.swap_chain_height =
                                direct_frame.height;
                            diagnostics_.output_target =
                                OutputTargetName(direct_target);
                            diagnostics_.swap_chain_format =
                                direct_frame.dxgi_format ==
                                        DXGI_FORMAT_R16G16B16A16_FLOAT
                                    ? "R16G16B16A16_FLOAT"
                                    : "B8G8R8A8_UNORM";
                            diagnostics_.color_space =
                                direct_frame.color_space ==
                                        DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                    ? "RGB_FULL_G10_NONE_P709"
                                    : "RGB_FULL_G22_NONE_P709";
                            diagnostics_.color_space_supported = true;
                            diagnostics_.sdr_tone_map_active =
                                direct_frame.dxgi_format !=
                                DXGI_FORMAT_R16G16B16A16_FLOAT;
                            diagnostics_.retained_graph_mode =
                                "d3d12-direct-present";
                            ++diagnostics_.present_count;
                            ++diagnostics_.composite_count;
                        }
                        release_held_video();
                        release_held_sdr_video();
                        return true;
                    }
                    spdlog::warn(
                        "[WindowsNativeCompositor] D3D12 direct present "
                        "failed: {}",
                        d3d12_present_target_->last_error());
                    d3d12_present_target_->shutdown();
                } else {
                    spdlog::debug(
                        "[WindowsNativeCompositor] D3D12 direct draw "
                        "deferred: renderer rejected external target");
                }
            }
        }
    }
d3d11_compositor_fallback:
    if (!legacy_inputs_preacquired) {
        acquire_legacy_video_inputs();
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
    const bool needs_sdr_video =
        composite_target == OutputTarget::SDR &&
        !held_sdr_video_valid_;
    if ((needs_fp16_video && !held_video_valid_) ||
        (needs_sdr_video && !held_video_valid_)) {
        return false;
    }
    acquire_finished = std::chrono::steady_clock::now();
    if (pending_swap_chain_.swap_chain) {
        uint64_t min_video_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            min_video_generation = transition_min_video_generation_;
        }
        const uint64_t held_video_generation =
            composite_target == OutputTarget::ScRGB
                ? held_video_.frame_generation
                : (held_sdr_video_valid_
                       ? held_sdr_video_.buffer_generation
                       : held_video_.frame_generation);
        if (held_video_generation < min_video_generation) {
            const auto now = std::chrono::steady_clock::now();
            if (last_transition_guard_log_.time_since_epoch().count() == 0 ||
                now - last_transition_guard_log_ >=
                    std::chrono::milliseconds(500)) {
                last_transition_guard_log_ = now;
                spdlog::info(
                    "[WindowsNativeCompositor] waiting for output target "
                    "transition inputs target={} video={}/{}",
                    OutputTargetName(composite_target),
                    held_video_generation,
                    min_video_generation);
            }
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
                    : (held_sdr_video_valid_
                           ? held_sdr_video_.width
                           : held_video_.width)));
            const auto video_height = static_cast<uint32_t>(std::max(
                0,
                composite_target == OutputTarget::ScRGB
                    ? held_video_.height
                    : (held_sdr_video_valid_
                           ? held_sdr_video_.height
                           : held_video_.height)));
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
            held_sdr_video_valid_
                ? held_sdr_video_srv_.Get()
                : held_video_srv_.Get(),
        };
        context_->PSSetShaderResources(0, 2, srvs);
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
        values.sdr_video_is_scrgb =
            output->target == OutputTarget::SDR && !held_sdr_video_valid_ &&
                    held_video_valid_
                ? 1.0f
                : 0.0f;
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
        ID3D11Buffer* constants = constants_.Get();
        context_->PSSetConstantBuffers(0, 1, &constants);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context_->PSSetShader(video_pixel_shader_.Get(), nullptr, 0);
        context_->Draw(4, 0);
        std::array<ID3D11ShaderResourceView*, 2> null_srvs = {};
        context_->PSSetShaderResources(
            0, static_cast<UINT>(null_srvs.size()), null_srvs.data());
        context_->Flush();
        const auto present_started = std::chrono::steady_clock::now();
        const UINT sync_interval = 1u;
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
        diagnostics_.flutter_generation =
            held_flutter_.frame_generation;
        diagnostics_.video_generation =
            composite_target == OutputTarget::ScRGB
                ? held_video_.frame_generation
                : (held_sdr_video_valid_
                       ? held_sdr_video_.buffer_generation
                       : held_video_.frame_generation);
        diagnostics_.source_projection_enabled = false;
        diagnostics_.source_cache_active = false;
        diagnostics_.retained_graph_active = false;
        diagnostics_.retained_graph_mode = "inactive";
        diagnostics_.retained_graph_fallback_reason =
            retained_graph_fallback_reason_;
        diagnostics_.retained_graph_commit_count = 0;
        diagnostics_.retained_graph_projection_commit_count = 0;
        diagnostics_.retained_graph_source_bake_count = 0;
        diagnostics_.retained_graph_flutter_bake_count = 0;
        diagnostics_.retained_graph_projection_skip_present_count = 0;
        diagnostics_.retained_graph_deferred_content_count = 0;
        diagnostics_.retained_graph_commit_defer_count = 0;
        const bool transition_inputs_ready =
            diagnostics_.video_generation >= transition_min_video_generation_;
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
