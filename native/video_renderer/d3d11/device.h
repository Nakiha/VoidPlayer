#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>

namespace vr {

class D3D11Device {
public:
    D3D11Device();
    ~D3D11Device();

    bool initialize(void* hwnd, int width, int height);
    void shutdown();

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    IDXGISwapChain* swap_chain() const { return swap_chain_.Get(); }

    void resize(int width, int height);
    void present(int sync_interval = 1);

private:
    void dump_debug_messages();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
    void* hwnd_ = nullptr;
    bool initialized_ = false;
};

} // namespace vr
