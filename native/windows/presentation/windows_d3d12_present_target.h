#pragma once

#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace vr {

enum class WindowsD3D12PresentTargetFormat {
    SDR,
    ScRGB,
};

struct WindowsD3D12PresentTargetFrame {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t buffer_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE color_space =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
};

class WindowsD3D12PresentTarget {
public:
    WindowsD3D12PresentTarget() = default;
    ~WindowsD3D12PresentTarget();

    WindowsD3D12PresentTarget(const WindowsD3D12PresentTarget&) = delete;
    WindowsD3D12PresentTarget& operator=(const WindowsD3D12PresentTarget&) =
        delete;

    bool initialize(HWND hwnd,
                    ID3D12Device* device,
                    ID3D12CommandQueue* queue,
                    uint32_t width,
                    uint32_t height,
                    WindowsD3D12PresentTargetFormat format);
    bool initialize_with_composition_visual(
        IDCompositionDevice* dcomp_device,
        IDCompositionTarget* dcomp_target,
        IDCompositionVisual* dcomp_visual,
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        uint32_t width,
        uint32_t height,
        WindowsD3D12PresentTargetFormat format);
    void shutdown();
    bool resize(uint32_t width,
                uint32_t height,
                WindowsD3D12PresentTargetFormat format);
    bool set_client_size(uint32_t width, uint32_t height);

    bool acquire_frame(WindowsD3D12PresentTargetFrame& frame);
    bool present(UINT sync_interval);
    bool present_after_external_render(
        const WindowsD3D12PresentTargetFrame& frame,
        UINT sync_interval);
    bool clear_and_present(const float color[4], UINT sync_interval);

    bool active() const {
        return swap_chain_ != nullptr && command_allocator_ != nullptr &&
               command_list_ != nullptr;
    }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    DXGI_FORMAT dxgi_format() const { return dxgi_format_; }
    const std::string& last_error() const { return last_error_; }

private:
    bool fail(const char* stage, HRESULT hr);
    bool wait_for_gpu();

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> command_allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    uint64_t fence_value_ = 0;
    uint32_t rtv_descriptor_size_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t visual_client_width_ = 0;
    uint32_t visual_client_height_ = 0;
    uint32_t visual_target_width_ = 0;
    uint32_t visual_target_height_ = 0;
    DXGI_FORMAT dxgi_format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::string last_error_ = "not-initialized";
};

} // namespace vr
