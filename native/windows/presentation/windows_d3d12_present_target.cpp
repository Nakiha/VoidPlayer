#include "windows/presentation/windows_d3d12_present_target.h"

#include <algorithm>
#include <cstdio>

namespace {

constexpr UINT kBufferCount = 3;

std::string format_hresult(const char* stage, HRESULT hr) {
    char buffer[128] = {};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s failed hr=0x%08x",
                  stage,
                  static_cast<unsigned>(hr));
    return buffer;
}

} // namespace

namespace vr {

WindowsD3D12PresentTarget::~WindowsD3D12PresentTarget() {
    shutdown();
}

bool WindowsD3D12PresentTarget::initialize(
    HWND hwnd,
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    uint32_t width,
    uint32_t height,
    WindowsD3D12PresentTargetFormat format) {
    shutdown();
    if (!hwnd || !device || !queue) {
        last_error_ = "d3d12-present-target-invalid-arguments";
        return false;
    }
    width_ = std::max(width, 1u);
    height_ = std::max(height, 1u);
    dxgi_format_ = format == WindowsD3D12PresentTargetFormat::ScRGB
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_B8G8R8A8_UNORM;
    color_space_ = format == WindowsD3D12PresentTargetFormat::ScRGB
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    device_ = device;
    queue_ = queue;

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return fail("CreateDXGIFactory2", hr);
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = dxgi_format_;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
    hr = factory->CreateSwapChainForComposition(
        queue_.Get(), &desc, nullptr, &chain);
    if (FAILED(hr) || !chain) {
        return fail("CreateSwapChainForComposition(D3D12)", hr);
    }
    hr = chain.As(&swap_chain_);
    if (FAILED(hr) || !swap_chain_) {
        return fail("Query IDXGISwapChain3", hr);
    }
    UINT color_support = 0;
    hr = swap_chain_->CheckColorSpaceSupport(color_space_, &color_support);
    if (FAILED(hr) ||
        (color_support &
         DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
        return fail("CheckColorSpaceSupport", FAILED(hr) ? hr : E_FAIL);
    }
    hr = swap_chain_->SetColorSpace1(color_space_);
    if (FAILED(hr)) {
        return fail("SetColorSpace1", hr);
    }

    hr = DCompositionCreateDevice2(
        nullptr, IID_PPV_ARGS(&dcomp_device_));
    if (FAILED(hr) || !dcomp_device_) {
        return fail("DCompositionCreateDevice2", hr);
    }
    hr = dcomp_device_->CreateTargetForHwnd(hwnd, TRUE, &dcomp_target_);
    if (FAILED(hr) || !dcomp_target_) {
        return fail("CreateTargetForHwnd", hr);
    }
    hr = dcomp_device_->CreateVisual(&dcomp_visual_);
    if (FAILED(hr) || !dcomp_visual_) {
        return fail("CreateVisual", hr);
    }
    hr = dcomp_visual_->SetContent(swap_chain_.Get());
    if (FAILED(hr)) {
        return fail("SetContent(D3D12 swapchain)", hr);
    }
    hr = dcomp_target_->SetRoot(dcomp_visual_.Get());
    if (FAILED(hr)) {
        return fail("SetRoot", hr);
    }
    hr = dcomp_device_->Commit();
    if (FAILED(hr)) {
        return fail("DComp Commit", hr);
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = kBufferCount;
    hr = device_->CreateDescriptorHeap(
        &heap_desc, IID_PPV_ARGS(&rtv_heap_));
    if (FAILED(hr) || !rtv_heap_) {
        return fail("CreateDescriptorHeap(RTV)", hr);
    }
    rtv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    hr = device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&command_allocator_));
    if (FAILED(hr) || !command_allocator_) {
        return fail("CreateCommandAllocator", hr);
    }
    hr = device_->CreateCommandList(0,
                                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    command_allocator_.Get(),
                                    nullptr,
                                    IID_PPV_ARGS(&command_list_));
    if (FAILED(hr) || !command_list_) {
        return fail("CreateCommandList", hr);
    }
    command_list_->Close();
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr) || !fence_) {
        return fail("CreateFence", hr);
    }
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        last_error_ = "CreateEventW failed";
        return false;
    }
    last_error_ = "none";
    return true;
}

void WindowsD3D12PresentTarget::shutdown() {
    if (queue_ && fence_) {
        wait_for_gpu();
    }
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    fence_.Reset();
    command_list_.Reset();
    command_allocator_.Reset();
    rtv_heap_.Reset();
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();
    swap_chain_.Reset();
    queue_.Reset();
    device_.Reset();
    width_ = 0;
    height_ = 0;
    dxgi_format_ = DXGI_FORMAT_UNKNOWN;
    color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    last_error_ = "not-initialized";
}

bool WindowsD3D12PresentTarget::clear_and_present(
    const float color[4],
    UINT sync_interval) {
    if (!device_ || !queue_ || !swap_chain_ || !rtv_heap_ ||
        !command_allocator_ || !command_list_) {
        last_error_ = "d3d12-present-target-not-initialized";
        return false;
    }
    WindowsD3D12PresentTargetFrame frame;
    if (!acquire_frame(frame)) {
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frame.buffer_index) * rtv_descriptor_size_;
    device_->CreateRenderTargetView(frame.resource.Get(), nullptr, rtv);

    HRESULT hr = command_allocator_->Reset();
    if (FAILED(hr)) {
        return fail("CommandAllocator::Reset", hr);
    }
    hr = command_list_->Reset(command_allocator_.Get(), nullptr);
    if (FAILED(hr)) {
        return fail("CommandList::Reset", hr);
    }

    D3D12_RESOURCE_BARRIER to_render = {};
    to_render.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_render.Transition.pResource = frame.resource.Get();
    to_render.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_render.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_render.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list_->ResourceBarrier(1, &to_render);
    command_list_->ClearRenderTargetView(rtv, color, 0, nullptr);
    D3D12_RESOURCE_BARRIER to_present = to_render;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    command_list_->ResourceBarrier(1, &to_present);
    hr = command_list_->Close();
    if (FAILED(hr)) {
        return fail("CommandList::Close", hr);
    }
    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    if (!wait_for_gpu()) {
        return false;
    }
    return present(sync_interval);
}

bool WindowsD3D12PresentTarget::acquire_frame(
    WindowsD3D12PresentTargetFrame& frame) {
    frame = {};
    if (!swap_chain_) {
        last_error_ = "d3d12-present-target-not-initialized";
        return false;
    }
    const UINT index = swap_chain_->GetCurrentBackBufferIndex();
    Microsoft::WRL::ComPtr<ID3D12Resource> back_buffer;
    HRESULT hr = swap_chain_->GetBuffer(index, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr) || !back_buffer) {
        return fail("GetBuffer", hr);
    }
    frame.resource = std::move(back_buffer);
    frame.buffer_index = index;
    frame.width = width_;
    frame.height = height_;
    frame.dxgi_format = dxgi_format_;
    frame.color_space = color_space_;
    last_error_ = "none";
    return true;
}

bool WindowsD3D12PresentTarget::present(UINT sync_interval) {
    if (!swap_chain_) {
        last_error_ = "d3d12-present-target-not-initialized";
        return false;
    }
    const HRESULT hr = swap_chain_->Present(sync_interval, 0);
    if (FAILED(hr)) {
        return fail("Present(D3D12)", hr);
    }
    last_error_ = "none";
    return true;
}

bool WindowsD3D12PresentTarget::fail(const char* stage, HRESULT hr) {
    last_error_ = format_hresult(stage, hr);
    return false;
}

bool WindowsD3D12PresentTarget::wait_for_gpu() {
    if (!queue_ || !fence_ || !fence_event_) {
        last_error_ = "wait-for-gpu-missing-state";
        return false;
    }
    const uint64_t value = ++fence_value_;
    HRESULT hr = queue_->Signal(fence_.Get(), value);
    if (FAILED(hr)) {
        return fail("Queue::Signal", hr);
    }
    if (fence_->GetCompletedValue() >= value) {
        return true;
    }
    hr = fence_->SetEventOnCompletion(value, fence_event_);
    if (FAILED(hr)) {
        return fail("Fence::SetEventOnCompletion", hr);
    }
    const DWORD wait = WaitForSingleObject(fence_event_, 1000);
    if (wait != WAIT_OBJECT_0) {
        last_error_ = "wait-for-gpu-timeout";
        return false;
    }
    return true;
}

} // namespace vr
