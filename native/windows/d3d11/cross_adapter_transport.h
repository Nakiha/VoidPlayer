#pragma once

#include <d3d11.h>
#include <d3d11_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vr {

enum class WindowsCrossAdapterSyncRequest {
    Auto,
    EventQuery,
    SharedFence,
};

const char* windows_cross_adapter_sync_request_name(
    WindowsCrossAdapterSyncRequest request);

WindowsCrossAdapterSyncRequest parse_windows_cross_adapter_sync_request(
    const char* value);

struct WindowsCrossAdapterTransportSupport {
    bool bgra8 = false;
    bool rgba16f = false;
    bool shared_fence = false;
    bool shared_fence_producer = false;
    bool shared_fence_output = false;
    bool shared_fence_handle_created = false;
    bool shared_fence_open_succeeded = false;
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
                    uint32_t height,
                    WindowsCrossAdapterSyncRequest sync_request =
                        WindowsCrossAdapterSyncRequest::Auto);
    void reset();

    bool copy_to_output_srv(ID3D11Texture2D* producer_texture,
                            ID3D11ShaderResourceView** srv_out);

    uint64_t copy_count() const { return copy_count_; }
    uint64_t bytes_per_copy() const { return bytes_per_copy_; }
    uint64_t timeout_count() const { return timeout_count_; }
    uint64_t last_copy_us() const { return last_copy_us_; }
    uint64_t total_copy_us() const { return total_copy_us_; }
    uint64_t shared_fence_signal_count() const {
        return shared_fence_signal_count_;
    }
    uint64_t shared_fence_wait_count() const {
        return shared_fence_wait_count_;
    }
    uint64_t shared_fence_timeout_count() const {
        return shared_fence_timeout_count_;
    }
    uint64_t shared_fence_last_wait_us() const {
        return shared_fence_last_wait_us_;
    }
    uint64_t shared_fence_p95_wait_us() const;
    uint64_t event_query_p95_wait_us() const;
    bool shared_fence_producer_supported() const {
        return shared_fence_producer_supported_;
    }
    bool shared_fence_output_supported() const {
        return shared_fence_output_supported_;
    }
    bool shared_fence_handle_created() const {
        return shared_fence_handle_created_;
    }
    bool shared_fence_open_succeeded() const {
        return shared_fence_open_succeeded_;
    }
    const std::string& requested_sync_kind() const {
        return requested_sync_kind_;
    }
    const std::string& active_sync_kind() const {
        return active_sync_kind_;
    }
    const std::string& sync_fallback_reason() const {
        return sync_fallback_reason_;
    }
    const std::string& last_error() const { return last_error_; }
    DXGI_FORMAT format() const { return format_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    bool wait_for_producer_copy();
    bool initialize_shared_fence();
    bool signal_shared_fence();
    bool wait_for_shared_fence();
    static uint64_t p95(std::vector<uint64_t> values);
    void record_event_query_wait(uint64_t wait_us);
    void record_shared_fence_wait(uint64_t wait_us);

    Microsoft::WRL::ComPtr<ID3D11Device> producer_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> producer_context_;
    Microsoft::WRL::ComPtr<ID3D11Device> output_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> output_context_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> producer_context4_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> output_context4_;
    Microsoft::WRL::ComPtr<ID3D11Fence> producer_fence_;
    Microsoft::WRL::ComPtr<ID3D11Fence> output_fence_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> producer_bridge_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_bridge_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_local_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> output_srv_;
    Microsoft::WRL::ComPtr<ID3D11Query> producer_event_query_;
    HANDLE shared_handle_ = nullptr;
    HANDLE shared_fence_handle_ = nullptr;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t fence_value_ = 0;
    uint64_t copy_count_ = 0;
    uint64_t bytes_per_copy_ = 0;
    uint64_t timeout_count_ = 0;
    uint64_t last_copy_us_ = 0;
    uint64_t total_copy_us_ = 0;
    uint64_t shared_fence_signal_count_ = 0;
    uint64_t shared_fence_wait_count_ = 0;
    uint64_t shared_fence_timeout_count_ = 0;
    uint64_t shared_fence_last_wait_us_ = 0;
    bool shared_fence_producer_supported_ = false;
    bool shared_fence_output_supported_ = false;
    bool shared_fence_handle_created_ = false;
    bool shared_fence_open_succeeded_ = false;
    std::string requested_sync_kind_ = "auto";
    std::string active_sync_kind_ = "event-query";
    std::string sync_fallback_reason_ = "none";
    std::string last_error_ = "none";
    std::vector<uint64_t> event_query_wait_samples_;
    std::vector<uint64_t> shared_fence_wait_samples_;
};

} // namespace vr
