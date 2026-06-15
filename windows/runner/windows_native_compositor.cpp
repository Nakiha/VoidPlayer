#include "windows_native_compositor.h"

#include "windows/presentation/windows_dcomp_composite.h"
#include "renderer/overlay/analysis_overlay_primitives.h"

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr int kExportDisabled = 0;
constexpr int kExportMirror = 1;
constexpr int kExportCompositorOwned = 2;

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
    float x = 0.0f;
    float y = 0.0f;
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

} // namespace

WindowsNativeCompositor::WindowsNativeCompositor() = default;

WindowsNativeCompositor::~WindowsNativeCompositor() {
    Stop();
}

bool WindowsNativeCompositor::Start(
    HWND hwnd,
    void* flutter_view,
    const std::shared_ptr<vr::NativePlayer>& player,
    IDXGIAdapter* adapter,
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
    const bool engine_api_available = LoadEngineApi();
    if (!hwnd_ || !flutter_view_ || !player || !engine_api_available) {
        spdlog::error(
            "[WindowsNativeCompositor] surface export unavailable: "
            "hwnd={} flutter_view={} player={} engine_api={}",
            hwnd_ != nullptr,
            flutter_view_ != nullptr,
            static_cast<bool>(player),
            engine_api_available);
        diagnostics_.fallback_reason = "flutter-surface-export-unavailable";
        return false;
    }
    if (!InitializeDeviceAndComposition(adapter) || !CreatePipeline()) {
        diagnostics_.fallback_reason = "dcomp-initialization-failed";
        return false;
    }

    diagnostics_.engine_export_available = true;
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
        fallback_finish_pending_ = false;
        rate_start_time_ = std::chrono::steady_clock::now();
        source_cache_publish_count_ = 0;
        source_cache_base_lease_wait_logged_ = false;
        source_cache_bundle_acquire_logged_ = false;
        source_cache_consumed_logged_ = false;
        diagnostics_.desired_output_target =
            OutputTargetName(output_target);
        diagnostics_.transition_reason = "initial";
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
    vertex_shader_.Reset();
    context_.Reset();
    device_.Reset();
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

void WindowsNativeCompositor::SetViewportRect(
    double left, double top, double right, double bottom) {
    std::lock_guard<std::mutex> lock(mutex_);
    viewport_[0] = std::clamp(left, 0.0, 1.0);
    viewport_[1] = std::clamp(top, 0.0, 1.0);
    viewport_[2] = std::clamp(right, viewport_[0], 1.0);
    viewport_[3] = std::clamp(bottom, viewport_[1], 1.0);
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

void WindowsNativeCompositor::SetSourceProjection(
    const SourceProjection& projection) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_projection_ = projection;
    diagnostics_.source_projection_enabled = projection.enabled;
    ++diagnostics_.source_projection_update_count;
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
    double sdr_white_level_nits,
    uint64_t display_generation,
    const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    const double next_scale =
        std::isfinite(sdr_white_level_nits) &&
                sdr_white_level_nits > 0.0
            ? sdr_white_level_nits / 80.0
            : 1.0;
    const bool target_changed = target != desired_output_target_;
    const bool white_changed =
        std::abs(
            next_scale -
            sdr_white_scale_.load(std::memory_order_relaxed)) > 0.0001;
    if (!target_changed && !white_changed &&
        display_generation == locked_display_generation_) {
        return;
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
        diagnostics_.video_generation + 1;
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
        } else if (!transparent_viewport &&
                   phase_ == Phase::FallbackRestoring) {
            fallback_finish_pending_ = true;
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
        PublishState(Phase::Active, "transparent-flutter-frame-acknowledged");
    }
}

void WindowsNativeCompositor::ForceFallbackForTesting(
    const std::string& reason) {
    EnterFallback(reason.empty() ? "ui-test-forced-fallback" : reason);
}

WindowsNativeCompositor::Diagnostics
WindowsNativeCompositor::diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Diagnostics result = diagnostics_;
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

void WindowsNativeCompositor::RequestDiagnosticCapture() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostic_capture_pending_ = true;
        work_pending_ = true;
    }
    wake_.notify_one();
}

void WindowsNativeCompositor::OnFlutterSurfacePublished(
    void*, uint64_t, void* user_data) {
    auto* compositor = static_cast<WindowsNativeCompositor*>(user_data);
    if (compositor) compositor->SignalWork();
}

bool WindowsNativeCompositor::LoadEngineApi() {
    HMODULE module = GetModuleHandleW(L"flutter_windows.dll");
    if (!module) return false;
    engine_api_.set_mode = reinterpret_cast<SetExportModeFn>(
        GetProcAddress(module, "FlutterDesktopViewSetSurfaceExportMode"));
    engine_api_.set_callback = reinterpret_cast<SetPublishedCallbackFn>(
        GetProcAddress(module, "FlutterDesktopViewSetSurfacePublishedCallback"));
    engine_api_.acquire = reinterpret_cast<AcquireFlutterSurfaceFn>(
        GetProcAddress(module, "FlutterDesktopViewAcquireLatestSurface"));
    engine_api_.release = reinterpret_cast<ReleaseFlutterSurfaceFn>(
        GetProcAddress(module, "FlutterDesktopViewReleaseSurface"));
    return engine_api_.available();
}

bool WindowsNativeCompositor::InitializeDeviceAndComposition(
    IDXGIAdapter* adapter) {
    const auto log_failure = [](const char* stage, HRESULT result) {
        spdlog::error(
            "[WindowsNativeCompositor] {} failed hr=0x{:08x}",
            stage,
            static_cast<uint32_t>(result));
        return false;
    };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = D3D11CreateDevice(
        adapter,
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
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
    return SUCCEEDED(hr) ||
           log_failure("CreateVisual", hr);
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

bool WindowsNativeCompositor::EnsureSwapChain(
    uint32_t width, uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    OutputTarget desired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        desired = desired_output_target_;
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
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,
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
        bool finish_fallback = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (diagnostics_.present_count == 0) {
                wake_.wait_until(
                    lock, first_present_deadline,
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
            finish_fallback = fallback_finish_pending_;
            fallback_finish_pending_ = false;
            first_present_timed_out =
                phase_ == Phase::Inactive &&
                diagnostics_.present_count == 0 &&
                std::chrono::steady_clock::now() >= first_present_deadline;
            work_pending_ = false;
        }
        if (finish_fallback) {
            if (auto player = player_.lock()) {
                ReleaseHeldInputs(player);
            }
            engine_api_.set_mode(flutter_view_, kExportDisabled);
            if (dcomp_target_) dcomp_target_->SetRoot(nullptr);
            if (dcomp_device_) dcomp_device_->Commit();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                terminal_inactive_ = true;
                diagnostics_.swap_chain_active = false;
            }
            PublishState(Phase::Inactive, "flutter-texture-restored");
            return;
        }
        if (first_present_timed_out) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                terminal_inactive_ = true;
                ++diagnostics_.failure_count;
                diagnostics_.fallback_reason = "first-dcomp-present-timeout";
            }
            engine_api_.set_mode(flutter_view_, kExportDisabled);
            PublishState(Phase::Inactive, "first-dcomp-present-timeout");
            return;
        }
        if (!CompositeLatest()) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++diagnostics_.drop_count;
        }
    }
}

bool WindowsNativeCompositor::CompositeLatest() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_inactive_) {
            return false;
        }
    }
    auto player = player_.lock();
    if (!player) return false;
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    if (FAILED(device_.As(&device1)) || !device1) {
        EnterFallback("dcomp-query-device1-failed");
        return false;
    }

    SourceProjection source_projection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_projection = source_projection_;
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
            const HRESULT open_result = device1->OpenSharedResource1(
                next_video.handle, IID_PPV_ARGS(&texture));
            if (SUCCEEDED(open_result) &&
                SUCCEEDED(texture.As(&keyed_mutex))) {
                acquired =
                    keyed_mutex->AcquireSync(
                        next_video.consumer_acquire_key, 8) == S_OK;
            }
            if (acquired &&
                SUCCEEDED(device_->CreateShaderResourceView(
                    texture.Get(), nullptr, &srv))) {
                release_held_video();
                held_video_ = next_video;
                held_video_texture_ = std::move(texture);
                held_video_mutex_ = std::move(keyed_mutex);
                held_video_srv_ = std::move(srv);
                held_video_valid_ = true;
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
            const HRESULT open_result = device_->OpenSharedResource(
                next_sdr_video.handle, IID_PPV_ARGS(&texture));
            if (SUCCEEDED(open_result) &&
                SUCCEEDED(device_->CreateShaderResourceView(
                    texture.Get(), nullptr, &srv))) {
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
            engine_api_.release(flutter_view_, next_flutter.lease_id);
        } else {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            bool acquired = false;
            const HRESULT open_result = device1->OpenSharedResource1(
                next_flutter.shared_texture_handle,
                IID_PPV_ARGS(&texture));
            if (SUCCEEDED(open_result) &&
                SUCCEEDED(texture.As(&keyed_mutex))) {
                acquired =
                    keyed_mutex->AcquireSync(
                        next_flutter.consumer_acquire_key, 8) == S_OK;
            }
            if (acquired &&
                SUCCEEDED(device_->CreateShaderResourceView(
                    texture.Get(), nullptr, &srv))) {
                release_held_flutter();
                held_flutter_ = next_flutter;
                held_flutter_texture_ = std::move(texture);
                held_flutter_mutex_ = std::move(keyed_mutex);
                held_flutter_srv_ = std::move(srv);
                held_flutter_valid_ = true;
            } else {
                if (acquired) {
                    keyed_mutex->ReleaseSync(
                        next_flutter.producer_release_key);
                }
                engine_api_.release(
                    flutter_view_, next_flutter.lease_id);
            }
        }
    }

    if (!held_video_valid_ || !held_sdr_video_valid_ ||
        !held_flutter_valid_) {
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
        EnterFallback("dcomp-swap-chain-create-or-resize-failed");
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
                    HRESULT result = device1->OpenSharedResource1(
                        source.handle,
                        IID_PPV_ARGS(&textures[slot]));
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
                    result = device_->CreateShaderResourceView(
                        textures[slot].Get(), nullptr, &srvs[slot]);
                    if (FAILED(result)) {
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
    if (pending_swap_chain_.swap_chain) {
        uint64_t min_video_generation = 0;
        uint64_t min_source_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            min_video_generation = transition_min_video_generation_;
            min_source_generation = transition_min_source_generation_;
        }
        if (held_video_.frame_generation < min_video_generation ||
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
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(back_desc.Width);
        viewport.Height = static_cast<float>(back_desc.Height);
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> current_rtv;
        if (FAILED(device_->CreateRenderTargetView(
                back_buffer.Get(), nullptr, &current_rtv))) {
            ok = false;
        }
        ID3D11RenderTargetView* rtv = current_rtv.Get();
        if (!ok) {
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
        const HRESULT present_result = output->swap_chain->Present(1, 0);
        ok = SUCCEEDED(present_result);
        if (!ok && device_->GetDeviceRemovedReason() != S_OK) {
            EnterFallback("dcomp-device-removed");
        }
    }

    if (!ok) {
        Phase phase;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            phase = phase_;
        }
        if (phase != Phase::FallbackRestoring) {
            EnterFallback("dcomp-composite-or-present-failed");
        }
        return false;
    }
    if (pending_swap_chain_.swap_chain &&
        !ActivatePendingSwapChain()) {
        EnterFallback("dcomp-swap-chain-activation-failed");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.flutter_generation =
            held_flutter_.frame_generation;
        diagnostics_.video_generation =
            held_video_.frame_generation;
        diagnostics_.source_projection_enabled =
            source_projection.enabled;
        diagnostics_.source_cache_active = source_bundle_active;
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
            held_video_.frame_generation >=
                transition_min_video_generation_ &&
            (!source_projection.enabled ||
             (source_bundle_active &&
              held_source_.frame_generation >=
                  transition_min_source_generation_));
        if (!pending_swap_chain_.swap_chain &&
            diagnostics_.transition_state == "preparing" &&
            transition_inputs_ready) {
            diagnostics_.transition_state = "stable";
            ++diagnostics_.output_generation;
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
    if (!overlay || overlay->empty() || back_desc.Width == 0 ||
        back_desc.Height == 0) {
        return true;
    }
    double viewport[4] = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::copy(std::begin(viewport_), std::end(viewport_), viewport);
    }
    const float viewport_left =
        static_cast<float>(viewport[0] * back_desc.Width);
    const float viewport_top =
        static_cast<float>(viewport[1] * back_desc.Height);
    const float viewport_width =
        static_cast<float>((viewport[2] - viewport[0]) * back_desc.Width);
    const float viewport_height =
        static_cast<float>((viewport[3] - viewport[1]) * back_desc.Height);
    if (viewport_width <= 0.0f || viewport_height <= 0.0f) {
        return true;
    }

    const int count = std::clamp(projection.active_track_count, 1, 4);
    const auto display_slot_for = [&](int source_slot) {
        for (int index = 0; index < count; ++index) {
            if (projection.source_order[static_cast<size_t>(index)] ==
                source_slot) {
                return index;
            }
        }
        return -1;
    };
    const auto project_uv = [&](int source_slot, float u, float v,
                                float& x, float& y) {
        const int display_slot = display_slot_for(source_slot);
        if (display_slot < 0) {
            return false;
        }
        const float inv_x = projection.inv_display_size_x[source_slot];
        const float inv_y = projection.inv_display_size_y[source_slot];
        if (std::abs(inv_x) < 0.00001f ||
            std::abs(inv_y) < 0.00001f) {
            return false;
        }
        float local_x =
            projection.display_offset_x[source_slot] +
            (u + projection.view_offset_uv_x[source_slot]) / inv_x;
        const float local_y =
            projection.display_offset_y[source_slot] +
            (v + projection.view_offset_uv_y[source_slot]) / inv_y;
        if (projection.mode == 0 && count > 1) {
            local_x = (display_slot + local_x) / count;
        }
        x = viewport_left + local_x * viewport_width;
        y = viewport_top + local_y * viewport_height;
        return true;
    };
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
    std::vector<OverlayVertex> vertices;
    const auto append_quad = [&](float left, float top, float right,
                                 float bottom, vr::analysis::OverlayColor color) {
        left = std::clamp(left, viewport_left, viewport_left + viewport_width);
        right = std::clamp(right, viewport_left, viewport_left + viewport_width);
        top = std::clamp(top, viewport_top, viewport_top + viewport_height);
        bottom = std::clamp(
            bottom, viewport_top, viewport_top + viewport_height);
        if (right <= left || bottom <= top || color.a == 0) {
            return;
        }
        const auto base = make_color(color);
        const auto vertex = [&](float px, float py) {
            OverlayVertex out = base;
            out.x = px / static_cast<float>(back_desc.Width) * 2.0f - 1.0f;
            out.y = 1.0f -
                    py / static_cast<float>(back_desc.Height) * 2.0f;
            return out;
        };
        const auto p0 = vertex(left, top);
        const auto p1 = vertex(right, top);
        const auto p2 = vertex(left, bottom);
        const auto p3 = vertex(right, bottom);
        vertices.insert(
            vertices.end(), {p0, p2, p1, p1, p2, p3});
    };
    const auto append_rect = [&](int source_slot,
                                 int video_width,
                                 int video_height,
                                 const vr::AnalysisOverlayRectPrimitive& rect,
                                 bool outline) {
        if (video_width <= 0 || video_height <= 0) {
            return;
        }
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
        if (!project_uv(
                source_slot,
                static_cast<float>(rect.x0) / video_width,
                static_cast<float>(rect.y0) / video_height,
                x0,
                y0) ||
            !project_uv(
                source_slot,
                static_cast<float>(rect.x1) / video_width,
                static_cast<float>(rect.y1) / video_height,
                x1,
                y1)) {
            return;
        }
        if (!outline) {
            append_quad(x0, y0, x1, y1, rect.color);
            return;
        }
        append_quad(x0, y0, x1, y0 + 1.0f, rect.color);
        append_quad(x0, y1 - 1.0f, x1, y1, rect.color);
        append_quad(x0, y0, x0 + 1.0f, y1, rect.color);
        append_quad(x1 - 1.0f, y0, x1, y1, rect.color);
    };
    const auto append_line = [&](int source_slot,
                                 int video_width,
                                 int video_height,
                                 const vr::AnalysisOverlayLinePrimitive& line) {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
        if (video_width <= 0 || video_height <= 0 ||
            !project_uv(
                source_slot,
                static_cast<float>(line.x0) / video_width,
                static_cast<float>(line.y0) / video_height,
                x0,
                y0) ||
            !project_uv(
                source_slot,
                static_cast<float>(line.x1) / video_width,
                static_cast<float>(line.y1) / video_height,
                x1,
                y1)) {
            return;
        }
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float length = std::max(std::sqrt(dx * dx + dy * dy), 0.0001f);
        const float nx = -dy / length * 0.5f;
        const float ny = dx / length * 0.5f;
        const auto base = make_color(line.color);
        const auto vertex = [&](float px, float py) {
            OverlayVertex out = base;
            out.x = px / static_cast<float>(back_desc.Width) * 2.0f - 1.0f;
            out.y = 1.0f -
                    py / static_cast<float>(back_desc.Height) * 2.0f;
            return out;
        };
        const auto p0 = vertex(x0 + nx, y0 + ny);
        const auto p1 = vertex(x0 - nx, y0 - ny);
        const auto p2 = vertex(x1 + nx, y1 + ny);
        const auto p3 = vertex(x1 - nx, y1 - ny);
        vertices.insert(
            vertices.end(), {p0, p1, p2, p2, p1, p3});
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
    if (vertices.empty()) {
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
    if (FAILED(device_->CreateBuffer(&desc, &data, &buffer))) {
        return false;
    }
    UINT stride = sizeof(OverlayVertex);
    UINT offset = 0;
    ID3D11Buffer* raw_buffer = buffer.Get();
    context_->IASetVertexBuffers(0, 1, &raw_buffer, &stride, &offset);
    context_->IASetInputLayout(overlay_input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(overlay_vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(overlay_pixel_shader_.Get(), nullptr, 0);
    context_->OMSetBlendState(
        overlay_blend_state_.Get(), nullptr, 0xffffffff);
    context_->Draw(static_cast<UINT>(vertices.size()), 0);
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
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

void WindowsNativeCompositor::EnterFallback(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::FallbackRestoring) {
            return;
        }
        ++diagnostics_.failure_count;
        diagnostics_.fallback_reason = reason;
    }
    if (auto player = player_.lock()) {
        player->clear_source_cache(reason.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_projection_ = {};
        diagnostics_.source_projection_enabled = false;
        diagnostics_.source_cache_active = false;
    }
    engine_api_.set_mode(flutter_view_, kExportMirror);
    PublishState(Phase::FallbackRestoring, reason);
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
        if (phase == Phase::FallbackRestoring) {
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
    case Phase::FallbackRestoring: return "fallback-restoring";
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
