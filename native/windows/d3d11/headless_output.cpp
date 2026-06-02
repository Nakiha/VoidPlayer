#include "windows/d3d11/headless_output.h"
#include "windows/d3d11/memory_estimate.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
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
        shutdown();
        return false;
    }
    buffers_.front.store(0);
    std::fill_n(buffers_.in_flight_count, kBufferCount, 0u);
    ++buffers_.generation;
    current_back_ = pick_free_buffer_locked();

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
    for (int i = 0; i < kBufferCount; ++i) {
        buffers_.textures[i].Reset();
        buffers_.rtvs[i].Reset();
        buffers_.handles[i] = {};
        buffers_.in_flight_count[i] = 0;
    }
    buffers_.front.store(0);
    ++buffers_.generation;
    gpu_fence_.Reset();
    frame_callback_ = nullptr;
    current_back_ = -1;
    device_ = nullptr;
    context_ = nullptr;
}

void D3D11HeadlessOutput::fail_shared_handle_for_test(bool enabled) {
    fail_shared_handle_for_test_ = enabled;
}

ID3D11Texture2D* D3D11HeadlessOutput::shared_texture_locked() const {
    return buffers_.textures[buffers_.front.load()].Get();
}

HANDLE D3D11HeadlessOutput::shared_texture_handle_locked() const {
    return buffers_.handles[buffers_.front.load()];
}

bool D3D11HeadlessOutput::acquire_shared_texture_locked(
    D3D11HeadlessOutputTextureLease& lease) {
    lease = {};

    const int front = buffers_.front.load();
    if (front < 0 || front >= kBufferCount) {
        return false;
    }

    ID3D11Texture2D* texture = buffers_.textures[front].Get();
    HANDLE handle = buffers_.handles[front];
    if (!texture || !handle) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    texture->AddRef();
    ++buffers_.in_flight_count[front];

    lease.texture = texture;
    lease.handle = handle;
    lease.width = static_cast<int>(desc.Width);
    lease.height = static_cast<int>(desc.Height);
    lease.buffer_index = front;
    lease.generation = buffers_.generation;
    return true;
}

void D3D11HeadlessOutput::release_shared_texture(int buffer_index, uint64_t generation) {
    if (buffer_index < 0 || buffer_index >= kBufferCount) {
        return;
    }

    std::lock_guard<std::mutex> lock(texture_mutex_);
    if (generation != buffers_.generation) {
        return;
    }
    if (buffers_.in_flight_count[buffer_index] > 0) {
        --buffers_.in_flight_count[buffer_index];
    }
}

bool D3D11HeadlessOutput::buffer_in_flight_for_test(int buffer_index) const {
    if (buffer_index < 0 || buffer_index >= kBufferCount) {
        return false;
    }
    std::lock_guard<std::mutex> lock(texture_mutex_);
    return buffers_.in_flight_count[buffer_index] > 0;
}

ID3D11RenderTargetView* D3D11HeadlessOutput::begin_frame_locked() {
    current_back_ = pick_free_buffer_locked();
    if (current_back_ < 0) {
        spdlog::debug("[D3D11HeadlessOutput] no free shared texture buffer; dropping frame");
        return nullptr;
    }
    return buffers_.rtvs[current_back_].Get();
}

std::function<void()> D3D11HeadlessOutput::publish_frame_locked() {
    if (current_back_ < 0 || current_back_ >= kBufferCount) {
        return {};
    }
    buffers_.front.store(current_back_);
    return frame_callback_;
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

bool D3D11HeadlessOutput::resize_locked(int width, int height) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> new_textures[kBufferCount];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> new_rtvs[kBufferCount];
    HANDLE new_handles[kBufferCount] = {};
    if (!create_shared_buffers(width, height, new_textures, new_rtvs, new_handles)) {
        return false;
    }

    for (int i = 0; i < kBufferCount; ++i) {
        buffers_.textures[i] = std::move(new_textures[i]);
        buffers_.rtvs[i] = std::move(new_rtvs[i]);
        buffers_.handles[i] = new_handles[i];
        buffers_.in_flight_count[i] = 0;
    }
    buffers_.front.store(0);
    ++buffers_.generation;
    current_back_ = pick_free_buffer_locked();

    spdlog::info("[D3D11HeadlessOutput] resize complete: {}x{}, handles=[{}, {}, {}]",
                 width, height,
                 reinterpret_cast<uintptr_t>(buffers_.handles[0]),
                 reinterpret_cast<uintptr_t>(buffers_.handles[1]),
                 reinterpret_cast<uintptr_t>(buffers_.handles[2]));
    return true;
}

void D3D11HeadlessOutput::cleanup_expired_pending_buffers() {
    // Kept as a no-op compatibility hook for the render loop. Flutter texture
    // release callbacks now drive buffer availability via release_shared_texture().
}

bool D3D11HeadlessOutput::snapshot_front_buffer_locked(
    D3D11HeadlessOutputFrontBufferSnapshot& snapshot) const {
    snapshot = {};
    const int front = buffers_.front.load();
    if (front < 0 || front >= kBufferCount) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    auto texture = buffers_.textures[front];
    if (!texture) {
        return false;
    }
    texture->GetDesc(&desc);

    snapshot.texture = std::move(texture);
    snapshot.width = static_cast<int>(desc.Width);
    snapshot.height = static_cast<int>(desc.Height);
    return true;
}

bool D3D11HeadlessOutput::capture_front_buffer_snapshot(
    const D3D11HeadlessOutputFrontBufferSnapshot& snapshot,
    std::vector<uint8_t>& bgra,
    int& width,
    int& height) {
    if (!device_ || !context_ || !snapshot.texture) {
        return false;
    }

    width = snapshot.width;
    height = snapshot.height;
    if (width <= 0 || height <= 0) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    snapshot.texture->GetDesc(&desc);
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

    context_->CopyResource(staging.Get(), snapshot.texture.Get());
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
    std::lock_guard<std::mutex> lock(texture_mutex_);
    frame_callback_ = std::move(cb);
}

D3D11HeadlessOutputMemoryStats D3D11HeadlessOutput::memory_stats() const {
    std::lock_guard<std::mutex> lock(texture_mutex_);
    D3D11HeadlessOutputMemoryStats stats;
    for (int i = 0; i < kBufferCount; ++i) {
        if (!buffers_.textures[i]) {
            continue;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        buffers_.textures[i]->GetDesc(&desc);
        const uint64_t bytes = estimate_d3d11_texture_bytes(desc);
        stats.estimated_bytes += bytes;
        ++stats.buffer_count;
        if (stats.texture_bytes == 0) {
            stats.texture_bytes = bytes;
            stats.width = static_cast<int>(desc.Width);
            stats.height = static_cast<int>(desc.Height);
            stats.format = static_cast<int>(desc.Format);
        }
    }
    return stats;
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
        if (FAILED(hr) || !dxgi_resource) {
            spdlog::error("[D3D11HeadlessOutput] failed to query IDXGIResource[{}]: HRESULT {:#x}",
                          i, static_cast<unsigned long>(hr));
            return false;
        }
        hr = dxgi_resource->GetSharedHandle(&handles[i]);
        if (fail_shared_handle_for_test_) {
            handles[i] = nullptr;
            hr = E_FAIL;
        }
        if (FAILED(hr) || !handles[i]) {
            spdlog::error("[D3D11HeadlessOutput] failed to get shared handle[{}]: HRESULT {:#x}",
                          i, static_cast<unsigned long>(hr));
            return false;
        }
    }
    return true;
}

int D3D11HeadlessOutput::pick_free_buffer_locked() const {
    const int front = buffers_.front.load();
    for (int offset = 1; offset < kBufferCount; ++offset) {
        const int candidate = (front + offset) % kBufferCount;
        if (candidate != front && buffers_.in_flight_count[candidate] == 0) {
            return candidate;
        }
    }
    return -1;
}

} // namespace vr
