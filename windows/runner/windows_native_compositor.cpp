#include "windows_native_compositor.h"

#include "windows/presentation/windows_dcomp_composite.h"

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
    float padding[3];
};

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
    if (!hwnd_ || !flutter_view_ || !player || !LoadEngineApi()) {
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
        work_pending_ = true;
        terminal_inactive_ = false;
        fallback_finish_pending_ = false;
    }
    thread_ = std::thread(&WindowsNativeCompositor::ThreadMain, this);
    return true;
}

void WindowsNativeCompositor::Stop(const char* reason) {
    const bool had_state =
        thread_.joinable() || flutter_view_ != nullptr ||
        static_cast<bool>(state_callback_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        work_pending_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (auto player = player_.lock()) {
        player->set_shared_fp16_frame_callback({});
    }
    if (engine_api_.available() && flutter_view_) {
        engine_api_.set_callback(flutter_view_, nullptr, nullptr);
        engine_api_.set_mode(flutter_view_, kExportDisabled);
    }
    if (dcomp_target_) dcomp_target_->SetRoot(nullptr);
    if (dcomp_device_) dcomp_device_->Commit();
    back_buffer_rtv_.Reset();
    swap_chain_.Reset();
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();
    constants_.Reset();
    sampler_.Reset();
    pixel_shader_.Reset();
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
    return diagnostics_;
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
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = D3D11CreateDevice(
        adapter,
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
        &device_, &level, &context_);
    if (FAILED(hr)) return false;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_.As(&dxgi_device))) return false;
    hr = DCompositionCreateDevice(
        dxgi_device.Get(), IID_PPV_ARGS(&dcomp_device_));
    if (FAILED(hr)) return false;
    hr = dcomp_device_->CreateTargetForHwnd(hwnd_, TRUE, &dcomp_target_);
    if (FAILED(hr)) return false;
    hr = dcomp_device_->CreateVisual(&dcomp_visual_);
    return SUCCEEDED(hr);
}

bool WindowsNativeCompositor::CreateSwapChain(
    uint32_t width, uint32_t height) {
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
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
    HRESULT hr = factory->CreateSwapChainForComposition(
        device_.Get(), &desc, nullptr, &chain);
    if (FAILED(hr) || FAILED(chain.As(&swap_chain_))) return false;
    swap_chain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
        FAILED(device_->CreateRenderTargetView(
            back_buffer.Get(), nullptr, &back_buffer_rtv_))) {
        return false;
    }
    if (FAILED(dcomp_visual_->SetContent(swap_chain_.Get())) ||
        FAILED(dcomp_target_->SetRoot(dcomp_visual_.Get())) ||
        FAILED(dcomp_device_->Commit())) {
        return false;
    }
    diagnostics_.swap_chain_active = true;
    diagnostics_.swap_chain_width = width;
    diagnostics_.swap_chain_height = height;
    return true;
}

bool WindowsNativeCompositor::EnsureSwapChainSize(
    uint32_t width, uint32_t height) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (!swap_chain_) {
        return CreateSwapChain(width, height);
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    if (FAILED(swap_chain_->GetDesc1(&desc))) {
        return false;
    }
    if (desc.Width == width && desc.Height == height) {
        return true;
    }
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->ClearState();
    context_->Flush();
    back_buffer_rtv_.Reset();
    HRESULT hr = swap_chain_->ResizeBuffers(
        3, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
    if (FAILED(hr)) {
        return false;
    }
    swap_chain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.swap_chain_width = width;
        diagnostics_.swap_chain_height = height;
        ++diagnostics_.resize_count;
        diagnostic_capture_pending_ = true;
    }
    spdlog::info(
        "[WindowsNativeCompositor] resized swap chain to {}x{}",
        width, height);
    return true;
}

bool WindowsNativeCompositor::CreatePipeline() {
    const char* shader = vr::windows_dcomp_composite_hlsl();
    const size_t shader_size = std::strlen(shader);
    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, &vs_blob, &errors);
    if (FAILED(hr)) return false;
    hr = D3DCompile(
        shader, shader_size, nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr)) return false;
    if (FAILED(device_->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
            nullptr, &vertex_shader_)) ||
        FAILED(device_->CreatePixelShader(
            ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
            nullptr, &pixel_shader_))) {
        return false;
    }
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
            if (stop_) return;
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
    vr::SharedFp16TextureSnapshot video;
    FlutterSurface flutter;
    if (!player->acquire_shared_fp16_texture(video) ||
        !engine_api_.acquire(flutter_view_, &flutter)) {
        if (video.handle) {
            player->release_shared_fp16_texture(
                video.buffer_index, video.ring_generation);
        }
        return false;
    }
    auto release_leases = [&]() {
        engine_api_.release(flutter_view_, flutter.lease_id);
        player->release_shared_fp16_texture(
            video.buffer_index, video.ring_generation);
    };
    if (!EnsureSwapChainSize(flutter.width, flutter.height)) {
        release_leases();
        EnterFallback("dcomp-swap-chain-create-or-resize-failed");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> video_texture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> flutter_texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> video_mutex;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> flutter_mutex;
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    bool video_acquired = false;
    bool flutter_acquired = false;
    const bool opened =
        SUCCEEDED(device_.As(&device1)) &&
        SUCCEEDED(device1->OpenSharedResource1(
            video.handle, IID_PPV_ARGS(&video_texture))) &&
        SUCCEEDED(device1->OpenSharedResource1(
            flutter.shared_texture_handle, IID_PPV_ARGS(&flutter_texture))) &&
        SUCCEEDED(video_texture.As(&video_mutex)) &&
        SUCCEEDED(flutter_texture.As(&flutter_mutex));
    if (opened) {
        video_acquired =
            video_mutex->AcquireSync(video.consumer_acquire_key, 8) == S_OK;
        if (video_acquired) {
            flutter_acquired =
                flutter_mutex->AcquireSync(
                    flutter.consumer_acquire_key, 8) == S_OK;
        }
    }
    if (!video_acquired || !flutter_acquired) {
        if (flutter_acquired) {
            flutter_mutex->ReleaseSync(flutter.producer_release_key);
        }
        if (video_acquired) {
            video_mutex->ReleaseSync(video.producer_release_key);
        }
        release_leases();
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> video_srv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> flutter_srv;
    bool ok =
        SUCCEEDED(device_->CreateShaderResourceView(
            video_texture.Get(), nullptr, &video_srv)) &&
        SUCCEEDED(device_->CreateShaderResourceView(
            flutter_texture.Get(), nullptr, &flutter_srv));
    if (ok) {
        D3D11_TEXTURE2D_DESC back_desc = {};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
        swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
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
            flutter_mutex->ReleaseSync(flutter.producer_release_key);
            video_mutex->ReleaseSync(video.producer_release_key);
            release_leases();
            return false;
        }
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = {video_srv.Get(), flutter_srv.Get()};
        context_->PSSetShaderResources(0, 2, srvs);
        ID3D11SamplerState* sampler = sampler_.Get();
        context_->PSSetSamplers(0, 1, &sampler);
        CompositeConstants values = {};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int i = 0; i < 4; ++i) {
                values.viewport[i] = static_cast<float>(viewport_[i]);
            }
        }
        values.sdr_white_scale = static_cast<float>(sdr_white_scale_);
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &values, 0, 0);
        ID3D11Buffer* constants = constants_.Get();
        context_->PSSetConstantBuffers(0, 1, &constants);
        context_->Draw(4, 0);
        std::array<ID3D11ShaderResourceView*, 2> null_srvs = {};
        context_->PSSetShaderResources(0, 2, null_srvs.data());
        context_->Flush();
        bool capture = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            capture = diagnostic_capture_pending_;
            diagnostic_capture_pending_ = false;
        }
        if (capture && !CaptureDiagnostics(
                           back_buffer.Get(), flutter_texture.Get())) {
            std::lock_guard<std::mutex> lock(mutex_);
            diagnostic_capture_pending_ = true;
        }
        const HRESULT present_result = swap_chain_->Present(1, 0);
        ok = SUCCEEDED(present_result);
        if (!ok && device_->GetDeviceRemovedReason() != S_OK) {
            EnterFallback("dcomp-device-removed");
        }
    }
    flutter_mutex->ReleaseSync(flutter.producer_release_key);
    video_mutex->ReleaseSync(video.producer_release_key);
    release_leases();

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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_.flutter_generation = flutter.frame_generation;
        diagnostics_.video_generation = video.frame_generation;
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
        const auto* row = reinterpret_cast<const uint16_t*>(
            static_cast<const uint8_t*>(final_map.pData) +
            static_cast<size_t>(y) * final_map.RowPitch);
        for (UINT x = 0; x < final_desc.Width; ++x) {
            const float r = vr::half_to_float(row[x * 4u]);
            const float g = vr::half_to_float(row[x * 4u + 1u]);
            const float b = vr::half_to_float(row[x * 4u + 2u]);
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
