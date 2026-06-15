#include "windows/d3d11/cross_adapter_transport.h"

#include <d3d11_3.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
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

} // namespace

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

    Microsoft::WRL::ComPtr<ID3D11Device5> producer5;
    Microsoft::WRL::ComPtr<ID3D11Device5> output5;
    support.shared_fence =
        SUCCEEDED(producer_device->QueryInterface(IID_PPV_ARGS(&producer5))) &&
        producer5 &&
        SUCCEEDED(output_device->QueryInterface(IID_PPV_ARGS(&output5))) &&
        output5;
    support.bgra8 = can_create_format_transport(
        producer_device, output_device, DXGI_FORMAT_B8G8R8A8_UNORM);
    support.rgba16f = can_create_format_transport(
        producer_device, output_device, DXGI_FORMAT_R16G16B16A16_FLOAT);
    support.sync_kind =
        support.shared_fence ? "shared-fence-capable-event-query"
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
    uint32_t height) {
    reset();
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

    D3D11_QUERY_DESC query_desc = {};
    query_desc.Query = D3D11_QUERY_EVENT;
    if (FAILED(producer_device_->CreateQuery(
            &query_desc, &producer_event_query_))) {
        last_error_ = "producer-event-query-create-failed";
        reset();
        return false;
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
    if (shared_handle_) {
        CloseHandle(shared_handle_);
        shared_handle_ = nullptr;
    }
    format_ = DXGI_FORMAT_UNKNOWN;
    width_ = 0;
    height_ = 0;
    bytes_per_copy_ = 0;
}

bool D3D11CrossAdapterTextureTransport::wait_for_producer_copy() {
    if (!producer_context_ || !producer_event_query_) {
        last_error_ = "producer-query-unavailable";
        return false;
    }
    constexpr int kMaxPolls = 200;
    for (int poll = 0; poll < kMaxPolls; ++poll) {
        const HRESULT hr = producer_context_->GetData(
            producer_event_query_.Get(), nullptr, 0, 0);
        if (hr == S_OK) {
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
    producer_context_->End(producer_event_query_.Get());
    producer_context_->Flush();
    if (!wait_for_producer_copy()) {
        return false;
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
