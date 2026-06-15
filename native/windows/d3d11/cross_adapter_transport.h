#pragma once

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace vr {

struct WindowsCrossAdapterTransportSupport {
    bool bgra8 = false;
    bool rgba16f = false;
    bool shared_fence = false;
    bool row_major = false;
    std::string sync_kind = "unavailable";
    std::string status = "unprobed";
};

bool windows_luid_equal(int32_t high_a,
                        uint32_t low_a,
                        int32_t high_b,
                        uint32_t low_b);

bool windows_cross_adapter_required(int32_t producer_high,
                                    uint32_t producer_low,
                                    int32_t output_high,
                                    uint32_t output_low);

WindowsCrossAdapterTransportSupport probe_windows_cross_adapter_transport(
    ID3D11Device* producer_device,
    ID3D11Device* output_device);

class D3D11CrossAdapterTextureTransport {
public:
    D3D11CrossAdapterTextureTransport() = default;
    ~D3D11CrossAdapterTextureTransport() = default;

    bool initialize(ID3D11Device* producer_device,
                    ID3D11DeviceContext* producer_context,
                    ID3D11Device* output_device,
                    ID3D11DeviceContext* output_context,
                    DXGI_FORMAT format,
                    uint32_t width,
                    uint32_t height);
    void reset();

    bool copy_to_output_srv(ID3D11Texture2D* producer_texture,
                            ID3D11ShaderResourceView** srv_out);

    uint64_t copy_count() const { return copy_count_; }
    uint64_t bytes_per_copy() const { return bytes_per_copy_; }
    uint64_t timeout_count() const { return timeout_count_; }
    uint64_t last_copy_us() const { return last_copy_us_; }
    uint64_t total_copy_us() const { return total_copy_us_; }
    const std::string& last_error() const { return last_error_; }
    DXGI_FORMAT format() const { return format_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    bool wait_for_producer_copy();

    Microsoft::WRL::ComPtr<ID3D11Device> producer_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> producer_context_;
    Microsoft::WRL::ComPtr<ID3D11Device> output_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> output_context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> producer_bridge_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_bridge_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_local_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv_;
    Microsoft::WRL::ComPtr<ID3D11Query> producer_event_query_;
    HANDLE shared_handle_ = nullptr;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t copy_count_ = 0;
    uint64_t bytes_per_copy_ = 0;
    uint64_t timeout_count_ = 0;
    uint64_t last_copy_us_ = 0;
    uint64_t total_copy_us_ = 0;
    std::string last_error_ = "none";
};

} // namespace vr
