#include "windows/d3d11/cross_adapter_transport.h"

#include <d3d11_3.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <thread>

namespace vr {
namespace {

uint64_t bytes_per_pixel(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    default:
        return 0;
    }
}

bool create_row_major_shared_texture(
    ID3D11Device* device,
    DXGI_FORMAT format,
    uint32_t width,
    uint32_t height,
    ID3D11Texture2D** texture_out,
    HANDLE* handle_out) {
    if (!device || !texture_out || !handle_out ||
        width == 0 || height == 0) {
        return false;
    }
    *texture_out = nullptr;
    *handle_out = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device3> device3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device3))) || !device3) {
        return false;
    }

    D3D11_TEXTURE2D_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    desc.TextureLayout = D3D11_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D11Texture2D1> texture1;
    HRESULT hr = device3->CreateTexture2D1(
        &desc, nullptr, texture1.GetAddressOf());
    if (FAILED(hr) || !texture1) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIResource1> resource;
    hr = texture1.As(&resource);
    if (FAILED(hr) || !resource) {
        return false;
    }
    HANDLE handle = nullptr;
    hr = resource->CreateSharedHandle(
        nullptr,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr,
        &handle);
    if (FAILED(hr) || !handle) {
        return false;
    }

    *handle_out = handle;
    *texture_out = texture1.Detach();
    return true;
}

bool can_create_format_transport(
    ID3D11Device* producer,
    ID3D11Device* output,
    DXGI_FORMAT format) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> producer_texture;
    HANDLE handle = nullptr;
    if (!create_row_major_shared_texture(
            producer, format, 16, 16,
            producer_texture.GetAddressOf(), &handle)) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Device1> output1;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> opened;
    const bool ok =
        SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output1))) &&
        output1 &&
        SUCCEEDED(output1->OpenSharedResource1(
            handle, IID_PPV_ARGS(&opened))) &&
        opened;
    CloseHandle(handle);
    return ok;
}

struct SharedFenceProbe {
    bool producer = false;
    bool output = false;
    bool handle_created = false;
    bool open_succeeded = false;
};

SharedFenceProbe probe_shared_fence(ID3D11Device* producer,
                                    ID3D11Device* output) {
    SharedFenceProbe result;
    if (!producer || !output) {
        return result;
    }
    Microsoft::WRL::ComPtr<ID3D11Device5> producer5;
    Microsoft::WRL::ComPtr<ID3D11Device5> output5;
    result.producer =
        SUCCEEDED(producer->QueryInterface(IID_PPV_ARGS(&producer5))) &&
        producer5;
    result.output =
        SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output5))) &&
        output5;
    if (!result.producer || !result.output) {
        return result;
    }
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    HRESULT hr = producer5->CreateFence(
        0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
    if (FAILED(hr) || !fence) {
        return result;
    }
    HANDLE handle = nullptr;
    hr = fence->CreateSharedHandle(
        nullptr,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr,
        &handle);
    result.handle_created = SUCCEEDED(hr) && handle;
    if (!result.handle_created) {
        return result;
    }
    Microsoft::WRL::ComPtr<ID3D11Fence> opened;
    hr = output5->OpenSharedFence(handle, IID_PPV_ARGS(&opened));
    result.open_succeeded = SUCCEEDED(hr) && opened;
    CloseHandle(handle);
    return result;
}

bool equals_ignore_case(const char* lhs, const char* rhs) {
    if (!lhs || !rhs) {
        return false;
    }
    while (*lhs && *rhs) {
        const char a = static_cast<char>(std::tolower(
            static_cast<unsigned char>(*lhs)));
        const char b = static_cast<char>(std::tolower(
            static_cast<unsigned char>(*rhs)));
        if (a != b) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

} // namespace

const char* windows_cross_adapter_sync_request_name(
    WindowsCrossAdapterSyncRequest request) {
    switch (request) {
    case WindowsCrossAdapterSyncRequest::Auto:
        return "auto";
    case WindowsCrossAdapterSyncRequest::EventQuery:
        return "event-query";
    case WindowsCrossAdapterSyncRequest::SharedFence:
        return "shared-fence";
    }
    return "auto";
}

WindowsCrossAdapterSyncRequest parse_windows_cross_adapter_sync_request(
    const char* value) {
    if (!value || value[0] == '\0' || equals_ignore_case(value, "auto")) {
        return WindowsCrossAdapterSyncRequest::Auto;
    }
    if (equals_ignore_case(value, "event-query")) {
        return WindowsCrossAdapterSyncRequest::EventQuery;
    }
    if (equals_ignore_case(value, "shared-fence")) {
        return WindowsCrossAdapterSyncRequest::SharedFence;
    }
    return WindowsCrossAdapterSyncRequest::Auto;
}

bool windows_luid_equal(int32_t high_a,
                        uint32_t low_a,
                        int32_t high_b,
                        uint32_t low_b) {
    return high_a == high_b && low_a == low_b;
}

bool windows_cross_adapter_required(int32_t producer_high,
                                    uint32_t producer_low,
                                    int32_t output_high,
                                    uint32_t output_low) {
    return !windows_luid_equal(
        producer_high, producer_low, output_high, output_low);
}

WindowsCrossAdapterTransportSupport probe_windows_cross_adapter_transport(
    ID3D11Device* producer_device,
    ID3D11Device* output_device) {
    WindowsCrossAdapterTransportSupport support;
    if (!producer_device || !output_device) {
        support.status = "missing-device";
        return support;
    }
    Microsoft::WRL::ComPtr<ID3D11Device3> producer3;
    Microsoft::WRL::ComPtr<ID3D11Device3> output3;
    support.row_major =
        SUCCEEDED(producer_device->QueryInterface(IID_PPV_ARGS(&producer3))) &&
        producer3 &&
        SUCCEEDED(output_device->QueryInterface(IID_PPV_ARGS(&output3))) &&
        output3;
    if (!support.row_major) {
        support.status = "d3d11-device3-unavailable";
        return support;
    }

    const auto fence = probe_shared_fence(producer_device, output_device);
    support.shared_fence_producer = fence.producer;
    support.shared_fence_output = fence.output;
    support.shared_fence_handle_created = fence.handle_created;
    support.shared_fence_open_succeeded = fence.open_succeeded;
    support.shared_fence =
        fence.producer && fence.output && fence.handle_created &&
        fence.open_succeeded;
    support.bgra8 = can_create_format_transport(
        producer_device, output_device, DXGI_FORMAT_B8G8R8A8_UNORM);
    support.rgba16f = can_create_format_transport(
        producer_device, output_device, DXGI_FORMAT_R16G16B16A16_FLOAT);
    support.sync_kind =
        support.shared_fence ? "event-query-shared-fence-capable"
                             : "event-query";
    support.status =
        support.bgra8 ? "ok" : "row-major-open-failed";
    return support;
}

bool D3D11CrossAdapterTextureTransport::initialize(
    ID3D11Device* producer_device,
    ID3D11DeviceContext* producer_context,
    ID3D11Device* output_device,
    ID3D11DeviceContext* output_context,
    DXGI_FORMAT format,
    uint32_t width,
    uint32_t height,
    WindowsCrossAdapterSyncRequest sync_request) {
    reset();
    requested_sync_kind_ =
        windows_cross_adapter_sync_request_name(sync_request);
    active_sync_kind_ = "event-query";
    sync_fallback_reason_ = "none";
    if (!producer_device || !producer_context || !output_device ||
        !output_context || width == 0 || height == 0 ||
        bytes_per_pixel(format) == 0) {
        last_error_ = "invalid-transport-config";
        return false;
    }

    producer_device_ = producer_device;
    producer_context_ = producer_context;
    output_device_ = output_device;
    output_context_ = output_context;
    format_ = format;
    width_ = width;
    height_ = height;
    bytes_per_copy_ =
        static_cast<uint64_t>(width_) * height_ * bytes_per_pixel(format_);

    if (!create_row_major_shared_texture(
            producer_device_.Get(), format_, width_, height_,
            producer_bridge_.GetAddressOf(), &shared_handle_)) {
        last_error_ = "row-major-shared-texture-create-failed";
        reset();
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device1> output1;
    if (FAILED(output_device_->QueryInterface(IID_PPV_ARGS(&output1))) ||
        !output1 ||
        FAILED(output1->OpenSharedResource1(
            shared_handle_, IID_PPV_ARGS(&output_bridge_))) ||
        !output_bridge_) {
        last_error_ = "row-major-shared-texture-open-failed";
        reset();
        return false;
    }

    D3D11_TEXTURE2D_DESC local_desc = {};
    local_desc.Width = width_;
    local_desc.Height = height_;
    local_desc.MipLevels = 1;
    local_desc.ArraySize = 1;
    local_desc.Format = format_;
    local_desc.SampleDesc.Count = 1;
    local_desc.Usage = D3D11_USAGE_DEFAULT;
    local_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(output_device_->CreateTexture2D(
            &local_desc, nullptr, &output_local_)) ||
        FAILED(output_device_->CreateShaderResourceView(
            output_local_.Get(), nullptr, &output_srv_))) {
        last_error_ = "output-local-texture-create-failed";
        reset();
        return false;
    }

    if (sync_request == WindowsCrossAdapterSyncRequest::SharedFence) {
        if (initialize_shared_fence()) {
            active_sync_kind_ = "shared-fence";
            spdlog::info(
                "[WindowsCrossAdapterTransport] using shared-fence sync "
                "format={} size={}x{}",
                static_cast<int>(format_), width_, height_);
        } else {
            sync_fallback_reason_ = last_error_;
            active_sync_kind_ = "event-query";
            spdlog::warn(
                "[WindowsCrossAdapterTransport] shared-fence unavailable; "
                "falling back to event-query reason={}",
                sync_fallback_reason_);
        }
    } else if (sync_request == WindowsCrossAdapterSyncRequest::Auto) {
        sync_fallback_reason_ = "auto-default-event-query";
    }

    if (active_sync_kind_ == "event-query") {
        D3D11_QUERY_DESC query_desc = {};
        query_desc.Query = D3D11_QUERY_EVENT;
        if (FAILED(producer_device_->CreateQuery(
                &query_desc, &producer_event_query_))) {
            last_error_ = "producer-event-query-create-failed";
            reset();
            return false;
        }
    }

    last_error_ = "none";
    return true;
}

void D3D11CrossAdapterTextureTransport::reset() {
    output_srv_.Reset();
    output_local_.Reset();
    output_bridge_.Reset();
    producer_event_query_.Reset();
    producer_bridge_.Reset();
    output_context_.Reset();
    output_device_.Reset();
    producer_context_.Reset();
    producer_device_.Reset();
    producer_context4_.Reset();
    output_context4_.Reset();
    producer_fence_.Reset();
    output_fence_.Reset();
    if (shared_handle_) {
        CloseHandle(shared_handle_);
        shared_handle_ = nullptr;
    }
    if (shared_fence_handle_) {
        CloseHandle(shared_fence_handle_);
        shared_fence_handle_ = nullptr;
    }
    format_ = DXGI_FORMAT_UNKNOWN;
    width_ = 0;
    height_ = 0;
    fence_value_ = 0;
    bytes_per_copy_ = 0;
    shared_fence_handle_created_ = false;
    shared_fence_open_succeeded_ = false;
}

bool D3D11CrossAdapterTextureTransport::wait_for_producer_copy() {
    if (!producer_context_ || !producer_event_query_) {
        last_error_ = "producer-query-unavailable";
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    constexpr int kMaxPolls = 200;
    for (int poll = 0; poll < kMaxPolls; ++poll) {
        const HRESULT hr = producer_context_->GetData(
            producer_event_query_.Get(), nullptr, 0, 0);
        if (hr == S_OK) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started);
            record_event_query_wait(static_cast<uint64_t>(
                std::max<int64_t>(0, elapsed.count())));
            return true;
        }
        if (hr != S_FALSE) {
            last_error_ = "producer-query-failed";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ++timeout_count_;
    last_error_ = "producer-copy-timeout";
    return false;
}

bool D3D11CrossAdapterTextureTransport::initialize_shared_fence() {
    shared_fence_producer_supported_ =
        SUCCEEDED(producer_context_->QueryInterface(
            IID_PPV_ARGS(&producer_context4_))) &&
        producer_context4_;
    shared_fence_output_supported_ =
        SUCCEEDED(output_context_->QueryInterface(
            IID_PPV_ARGS(&output_context4_))) &&
        output_context4_;
    Microsoft::WRL::ComPtr<ID3D11Device5> producer5;
    Microsoft::WRL::ComPtr<ID3D11Device5> output5;
    shared_fence_producer_supported_ =
        shared_fence_producer_supported_ &&
        SUCCEEDED(producer_device_->QueryInterface(IID_PPV_ARGS(&producer5))) &&
        producer5;
    shared_fence_output_supported_ =
        shared_fence_output_supported_ &&
        SUCCEEDED(output_device_->QueryInterface(IID_PPV_ARGS(&output5))) &&
        output5;
    if (!shared_fence_producer_supported_ ||
        !shared_fence_output_supported_) {
        last_error_ = "shared-fence-device5-or-context4-unavailable";
        return false;
    }
    HRESULT hr = producer5->CreateFence(
        0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&producer_fence_));
    if (FAILED(hr) || !producer_fence_) {
        last_error_ = "shared-fence-create-failed";
        return false;
    }
    hr = producer_fence_->CreateSharedHandle(
        nullptr,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr,
        &shared_fence_handle_);
    shared_fence_handle_created_ = SUCCEEDED(hr) && shared_fence_handle_;
    if (!shared_fence_handle_created_) {
        last_error_ = "shared-fence-handle-create-failed";
        return false;
    }
    hr = output5->OpenSharedFence(
        shared_fence_handle_, IID_PPV_ARGS(&output_fence_));
    shared_fence_open_succeeded_ = SUCCEEDED(hr) && output_fence_;
    if (!shared_fence_open_succeeded_) {
        last_error_ = "shared-fence-open-failed";
        return false;
    }
    return true;
}

bool D3D11CrossAdapterTextureTransport::signal_shared_fence() {
    if (!producer_context4_ || !producer_fence_) {
        last_error_ = "shared-fence-signal-unavailable";
        return false;
    }
    ++fence_value_;
    const HRESULT hr = producer_context4_->Signal(
        producer_fence_.Get(), fence_value_);
    if (FAILED(hr)) {
        last_error_ = "shared-fence-signal-failed";
        return false;
    }
    ++shared_fence_signal_count_;
    return true;
}

bool D3D11CrossAdapterTextureTransport::wait_for_shared_fence() {
    if (!output_context4_ || !output_fence_) {
        last_error_ = "shared-fence-wait-unavailable";
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    const HRESULT hr = output_context4_->Wait(
        output_fence_.Get(), fence_value_);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    record_shared_fence_wait(static_cast<uint64_t>(
        std::max<int64_t>(0, elapsed.count())));
    if (FAILED(hr)) {
        last_error_ = "shared-fence-wait-failed";
        return false;
    }
    ++shared_fence_wait_count_;
    return true;
}

uint64_t D3D11CrossAdapterTextureTransport::p95(
    std::vector<uint64_t> values) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = std::min(
        values.size() - 1,
        static_cast<size_t>(
            std::ceil(static_cast<double>(values.size()) * 0.95) - 1.0));
    return values[index];
}

void D3D11CrossAdapterTextureTransport::record_event_query_wait(
    uint64_t wait_us) {
    event_query_wait_samples_.push_back(wait_us);
    if (event_query_wait_samples_.size() > 256) {
        event_query_wait_samples_.erase(event_query_wait_samples_.begin());
    }
}

void D3D11CrossAdapterTextureTransport::record_shared_fence_wait(
    uint64_t wait_us) {
    shared_fence_last_wait_us_ = wait_us;
    shared_fence_wait_samples_.push_back(wait_us);
    if (shared_fence_wait_samples_.size() > 256) {
        shared_fence_wait_samples_.erase(shared_fence_wait_samples_.begin());
    }
}

uint64_t D3D11CrossAdapterTextureTransport::shared_fence_p95_wait_us() const {
    return p95(shared_fence_wait_samples_);
}

uint64_t D3D11CrossAdapterTextureTransport::event_query_p95_wait_us() const {
    return p95(event_query_wait_samples_);
}

bool D3D11CrossAdapterTextureTransport::copy_to_output_srv(
    ID3D11Texture2D* producer_texture,
    ID3D11ShaderResourceView** srv_out) {
    if (srv_out) {
        *srv_out = nullptr;
    }
    if (!producer_texture || !srv_out || !producer_bridge_ ||
        !output_bridge_ || !output_local_) {
        last_error_ = "transport-not-initialized";
        return false;
    }
    D3D11_TEXTURE2D_DESC source_desc = {};
    producer_texture->GetDesc(&source_desc);
    if (source_desc.Width != width_ || source_desc.Height != height_ ||
        source_desc.Format != format_) {
        last_error_ = "transport-source-shape-mismatch";
        return false;
    }

    const auto started = std::chrono::steady_clock::now();
    producer_context_->CopyResource(producer_bridge_.Get(), producer_texture);
    if (active_sync_kind_ == "shared-fence") {
        if (!signal_shared_fence()) {
            return false;
        }
        producer_context_->Flush();
        if (!wait_for_shared_fence()) {
            return false;
        }
    } else {
        producer_context_->End(producer_event_query_.Get());
        producer_context_->Flush();
        if (!wait_for_producer_copy()) {
            return false;
        }
    }
    output_context_->CopyResource(output_local_.Get(), output_bridge_.Get());
    output_context_->Flush();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    last_copy_us_ = static_cast<uint64_t>(
        std::max<int64_t>(0, elapsed.count()));
    total_copy_us_ += last_copy_us_;
    ++copy_count_;
    last_error_ = "none";
    output_srv_->AddRef();
    *srv_out = output_srv_.Get();
    return true;
}

} // namespace vr
