#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <memory>

namespace vr {

class D3D11Device {
public:
    D3D11Device();
    ~D3D11Device();

    /// Initialize with swap chain (standalone mode).
    bool initialize(void* hwnd, int width, int height);

    /// Initialize without swap chain (headless/texture-sharing mode).
    /// Uses the given DXGI adapter to ensure same-device texture sharing.
    bool initialize_headless(IDXGIAdapter* adapter, int width, int height);

    void shutdown();

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    IDXGISwapChain* swap_chain() const { return swap_chain_.Get(); }
    bool is_headless() const { return headless_; }
    bool device_lost() const { return device_lost_.load(std::memory_order_acquire); }
    HRESULT device_removed_reason() const {
        return device_removed_reason_.load(std::memory_order_acquire);
    }

    void resize(int width, int height);
    void present(int sync_interval = 1);

private:
    bool create_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type,
                       UINT flags, D3D_FEATURE_LEVEL& out_level);
    void setup_info_queue();
    void handle_device_error(const char* operation, HRESULT hr);

    void dump_debug_messages();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    void* hwnd_ = nullptr;
    bool initialized_ = false;
    bool headless_ = false;
    std::atomic<bool> device_lost_{false};
    std::atomic<HRESULT> device_removed_reason_{S_OK};
};

} // namespace vr
