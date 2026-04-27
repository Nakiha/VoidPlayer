#include "video_renderer/d3d11/headless_output.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>

namespace vr {

bool D3D11HeadlessOutput::initialize(ID3D11Device* device,
                                     ID3D11DeviceContext* context,
                                     int width,
                                     int height) {
    device_ = device;
    context_ = context;
    if (!device_ || !context_) {
        return false;
    }

    if (!create_shared_buffers(width, height,
                               buffers_.textures, buffers_.rtvs, buffers_.handles)) {
        return false;
    }
    buffers_.front.store(0);
    current_back_ = pick_free_buffer();

    D3D11_QUERY_DESC fence_desc = {};
    fence_desc.Query = D3D11_QUERY_EVENT;
    HRESULT hr = device_->CreateQuery(&fence_desc, &gpu_fence_);
    if (FAILED(hr)) {
        spdlog::error("[D3D11HeadlessOutput] Failed to create GPU fence: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        shutdown();
        return false;
    }

    spdlog::info("[D3D11HeadlessOutput] triple-buffered {}x{} BGRA, handles=[{}, {}, {}]",
                 width, height,
                 reinterpret_cast<uintptr_t>(buffers_.handles[0]),
                 reinterpret_cast<uintptr_t>(buffers_.handles[1]),
                 reinterpret_cast<uintptr_t>(buffers_.handles[2]));
    return true;
}

void D3D11HeadlessOutput::shutdown() {
    std::lock_guard<std::mutex> lock(texture_mutex_);
    pending_destroy_.clear();
    for (int i = 0; i < kBufferCount; ++i) {
        buffers_.textures[i].Reset();
        buffers_.rtvs[i].Reset();
        buffers_.handles[i] = {};
    }
    buffers_.front.store(0);
    gpu_fence_.Reset();
    frame_callback_ = nullptr;
    current_back_ = 0;
    device_ = nullptr;
    context_ = nullptr;
    has_pending_destroy_.store(false);
}

ID3D11Texture2D* D3D11HeadlessOutput::shared_texture() const {
    return buffers_.textures[buffers_.front.load()].Get();
}

HANDLE D3D11HeadlessOutput::shared_texture_handle() const {
    return buffers_.handles[buffers_.front.load()];
}

ID3D11RenderTargetView* D3D11HeadlessOutput::begin_frame() {
    current_back_ = pick_free_buffer();
    return buffers_.rtvs[current_back_].Get();
}

void D3D11HeadlessOutput::publish_frame(const char* label) {
    wait_gpu_idle(label);
    buffers_.front.store(current_back_);
    if (frame_callback_) {
        frame_callback_();
    }
}

void D3D11HeadlessOutput::wait_gpu_idle(const char* label) {
    if (!context_) {
        return;
    }
    if (!gpu_fence_) {
        context_->Flush();
        return;
    }

    context_->End(gpu_fence_.Get());
    auto fence_start = std::chrono::steady_clock::now();
    int spin_count = 0;
    while (context_->GetData(gpu_fence_.Get(), nullptr, 0, 0) == S_FALSE) {
        SwitchToThread();
        if (++spin_count >= 256) {
            spin_count = 0;
            if (std::chrono::steady_clock::now() - fence_start > std::chrono::milliseconds(100)) {
                spdlog::warn("[{}] GPU fence timeout after 100ms", label);
                break;
            }
        }
    }
}

bool D3D11HeadlessOutput::resize(int width, int height) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> new_textures[kBufferCount];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> new_rtvs[kBufferCount];
    HANDLE new_handles[kBufferCount] = {};
    if (!create_shared_buffers(width, height, new_textures, new_rtvs, new_handles)) {
        return false;
    }

    PendingBuffers old;
    for (int i = 0; i < kBufferCount; ++i) {
        old.textures[i] = std::move(buffers_.textures[i]);
        old.handles[i] = buffers_.handles[i];
    }
    old.expire_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    pending_destroy_.push_back(std::move(old));
    has_pending_destroy_.store(true);

    for (int i = 0; i < kBufferCount; ++i) {
        buffers_.textures[i] = std::move(new_textures[i]);
        buffers_.rtvs[i] = std::move(new_rtvs[i]);
        buffers_.handles[i] = new_handles[i];
    }
    buffers_.front.store(0);
    current_back_ = pick_free_buffer();

    spdlog::info("[D3D11HeadlessOutput] resize complete: {}x{}, handles=[{}, {}, {}]",
                 width, height,
                 reinterpret_cast<uintptr_t>(buffers_.handles[0]),
                 reinterpret_cast<uintptr_t>(buffers_.handles[1]),
                 reinterpret_cast<uintptr_t>(buffers_.handles[2]));
    return true;
}

void D3D11HeadlessOutput::cleanup_expired_pending_buffers() {
    if (!has_pending_destroy_.exchange(false)) {
        return;
    }
    std::lock_guard<std::mutex> lock(texture_mutex_);
    auto now = std::chrono::steady_clock::now();
    pending_destroy_.erase(
        std::remove_if(pending_destroy_.begin(), pending_destroy_.end(),
                       [&](const PendingBuffers& pb) { return now >= pb.expire_time; }),
        pending_destroy_.end());
    if (!pending_destroy_.empty()) {
        has_pending_destroy_.store(true);
    }
}

bool D3D11HeadlessOutput::capture_front_buffer(std::vector<uint8_t>& bgra,
                                               int& width,
                                               int& height) {
    if (!device_ || !context_) {
        return false;
    }

    const int front = buffers_.front.load();
    auto source = buffers_.textures[front];
    if (!source) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    width = static_cast<int>(desc.Width);
    height = static_cast<int>(desc.Height);

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        spdlog::error("[D3D11HeadlessOutput] capture_front_buffer: failed to create staging texture: {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    context_->CopyResource(staging.Get(), source.Get());
    context_->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        spdlog::error("[D3D11HeadlessOutput] capture_front_buffer: Map failed: {:#x}",
                      static_cast<unsigned long>(hr));
        return false;
    }

    bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    const size_t dst_stride = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        std::memcpy(bgra.data() + static_cast<size_t>(y) * dst_stride,
                    src + static_cast<size_t>(y) * mapped.RowPitch,
                    dst_stride);
    }

    context_->Unmap(staging.Get(), 0);
    return true;
}

void D3D11HeadlessOutput::set_frame_callback(std::function<void()> cb) {
    frame_callback_ = std::move(cb);
}

bool D3D11HeadlessOutput::create_shared_buffers(
    int width,
    int height,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[],
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[],
    HANDLE handles[]) {
    if (!device_) {
        return false;
    }

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = static_cast<UINT>(width);
    tex_desc.Height = static_cast<UINT>(height);
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    for (int i = 0; i < kBufferCount; ++i) {
        HRESULT hr = device_->CreateTexture2D(&tex_desc, nullptr, &textures[i]);
        if (FAILED(hr)) {
            spdlog::error("[D3D11HeadlessOutput] failed to create shared texture[{}]: HRESULT {:#x}",
                          i, static_cast<unsigned long>(hr));
            return false;
        }
        hr = device_->CreateRenderTargetView(textures[i].Get(), nullptr, &rtvs[i]);
        if (FAILED(hr)) {
            spdlog::error("[D3D11HeadlessOutput] failed to create shared RTV[{}]: HRESULT {:#x}",
                          i, static_cast<unsigned long>(hr));
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIResource> dxgi_resource;
        hr = textures[i].As(&dxgi_resource);
        if (SUCCEEDED(hr)) {
            hr = dxgi_resource->GetSharedHandle(&handles[i]);
            if (FAILED(hr)) {
                spdlog::warn("[D3D11HeadlessOutput] failed to get shared handle[{}]: HRESULT {:#x}",
                             i, static_cast<unsigned long>(hr));
            }
        }
    }
    return true;
}

int D3D11HeadlessOutput::pick_free_buffer() const {
    int front = buffers_.front.load();
    return (front + 2) % kBufferCount;
}

} // namespace vr
