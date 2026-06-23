#include "windows/d3d11/shared_fp16_ring.h"

#include "windows/d3d11/memory_estimate.h"

#include <algorithm>
#include <dxgi1_2.h>
#include <spdlog/spdlog.h>

namespace vr {

D3D11SharedFp16Ring::Slot::~Slot() {
    if (handle) {
        CloseHandle(handle);
    }
}

bool D3D11SharedFp16Ring::initialize(
    ID3D11Device* device, ID3D11DeviceContext* context,
    int width, int height) {
    shutdown();
    device_ = device;
    context_ = context;
    active_ = create_generation(width, height);
    return active_ != nullptr;
}

void D3D11SharedFp16Ring::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = {};
    writing_ = nullptr;
    latest_ = nullptr;
    active_.reset();
    prewarmed_.reset();
    latest_generation_.reset();
    retired_.clear();
    device_ = nullptr;
    context_ = nullptr;
}

bool D3D11SharedFp16Ring::prewarm(int width, int height) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++prewarm_request_count_;
        if ((active_ && active_->width == width && active_->height == height) ||
            (prewarmed_ && prewarmed_->width == width &&
             prewarmed_->height == height)) {
            ++prewarm_hit_count_;
            return true;
        }
    }

    auto replacement = create_generation(width, height);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!replacement) {
        ++prewarm_dropped_count_;
        return false;
    }
    if (prewarmed_) {
        ++prewarm_dropped_count_;
    }
    prewarmed_ = std::move(replacement);
    prewarm_ready_count_ += kBufferCount;
    collect_retired_locked();
    return true;
}

bool D3D11SharedFp16Ring::resize(int width, int height) {
    std::shared_ptr<Generation> replacement;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (prewarmed_ && prewarmed_->width == width &&
            prewarmed_->height == height) {
            replacement = std::move(prewarmed_);
            ++prewarm_consumed_count_;
        }
    }
    if (!replacement) {
        replacement = create_generation(width, height);
    }
    if (!replacement) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
        retired_.push_back(active_);
    }
    active_ = std::move(replacement);
    writing_ = nullptr;
    writing_index_ = -1;
    collect_retired_locked();
    return true;
}

ID3D11RenderTargetView* D3D11SharedFp16Ring::begin_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || writing_) {
        return nullptr;
    }
    for (int i = 0; i < kBufferCount; ++i) {
        auto* slot = active_->slots[i].get();
        if (!slot || slot->writing || slot->leases != 0 ||
            (latest_generation_ == active_ && latest_ == slot)) {
            continue;
        }
        if (slot->keyed_mutex->AcquireSync(0, 0) != S_OK) {
            continue;
        }
        slot->writing = true;
        writing_ = slot;
        writing_index_ = i;
        return slot->rtv.Get();
    }
    ++backpressure_count_;
    return nullptr;
}

bool D3D11SharedFp16Ring::publish_frame() {
    std::function<void()> callback;
    Slot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!writing_ || !active_) {
            return false;
        }
        slot = writing_;
        slot->writing = false;
        slot->frame_generation = next_frame_generation_++;
        latest_ = slot;
        latest_generation_ = active_;
        writing_ = nullptr;
        writing_index_ = -1;
        ++publish_count_;
        callback = callback_;
        collect_retired_locked();
    }
    if (FAILED(slot->keyed_mutex->ReleaseSync(1))) {
        return false;
    }
    if (callback) {
        callback();
    }
    return true;
}

void D3D11SharedFp16Ring::cancel_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!writing_) {
        return;
    }
    writing_->writing = false;
    writing_->keyed_mutex->ReleaseSync(0);
    writing_ = nullptr;
    writing_index_ = -1;
}

bool D3D11SharedFp16Ring::acquire_latest(
    SharedFp16TextureSnapshot& snapshot) {
    snapshot = {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_ || !latest_generation_ || latest_->writing) {
        return false;
    }
    ++latest_->leases;
    snapshot.handle = latest_->handle;
    snapshot.width = latest_generation_->width;
    snapshot.height = latest_generation_->height;
    snapshot.ring_generation = latest_generation_->id;
    snapshot.frame_generation = latest_->frame_generation;
    for (int i = 0; i < kBufferCount; ++i) {
        if (latest_generation_->slots[i].get() == latest_) {
            snapshot.buffer_index = i;
            break;
        }
    }
    return snapshot.handle != nullptr && snapshot.buffer_index >= 0;
}

void D3D11SharedFp16Ring::release(
    int buffer_index, uint64_t ring_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto release_from = [&](const std::shared_ptr<Generation>& generation) {
        if (!generation || generation->id != ring_generation ||
            buffer_index < 0 || buffer_index >= kBufferCount) {
            return false;
        }
        auto* slot = generation->slots[buffer_index].get();
        if (slot && slot->leases > 0) {
            --slot->leases;
        }
        return true;
    };
    if (!release_from(active_)) {
        for (const auto& generation : retired_) {
            if (release_from(generation)) {
                break;
            }
        }
    }
    collect_retired_locked();
}

void D3D11SharedFp16Ring::set_frame_callback(
    std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

uint64_t D3D11SharedFp16Ring::estimated_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t bytes = 0;
    const auto add_generation = [&bytes](const std::shared_ptr<Generation>& generation) {
        if (!generation) return;
        for (const auto& slot : generation->slots) {
            if (!slot || !slot->texture) continue;
            D3D11_TEXTURE2D_DESC desc = {};
            slot->texture->GetDesc(&desc);
            bytes += estimate_d3d11_texture_bytes(desc);
        }
    };
    add_generation(active_);
    add_generation(prewarmed_);
    for (const auto& generation : retired_) add_generation(generation);
    return bytes;
}

uint64_t D3D11SharedFp16Ring::publish_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publish_count_;
}

uint64_t D3D11SharedFp16Ring::backpressure_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backpressure_count_;
}

D3D11SharedFp16RingPrewarmStats D3D11SharedFp16Ring::prewarm_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return D3D11SharedFp16RingPrewarmStats{
        prewarm_request_count_,
        prewarm_ready_count_,
        prewarm_hit_count_,
        prewarm_dropped_count_,
        prewarm_consumed_count_,
    };
}

int D3D11SharedFp16Ring::width() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ ? active_->width : 0;
}

int D3D11SharedFp16Ring::height() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ ? active_->height : 0;
}

std::shared_ptr<D3D11SharedFp16Ring::Generation>
D3D11SharedFp16Ring::create_generation(int width, int height) {
    if (!device_ || width <= 0 || height <= 0) {
        return nullptr;
    }
    auto generation = std::make_shared<Generation>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation->id = next_ring_generation_++;
    }
    generation->width = width;
    generation->height = height;
    for (auto& slot_ptr : generation->slots) {
        auto slot = std::make_unique<Slot>();
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                         D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &slot->texture);
        if (FAILED(hr) || !slot->texture) return nullptr;
        hr = device_->CreateRenderTargetView(slot->texture.Get(), nullptr, &slot->rtv);
        if (FAILED(hr) || !slot->rtv) return nullptr;
        hr = slot->texture.As(&slot->keyed_mutex);
        if (FAILED(hr) || !slot->keyed_mutex) return nullptr;
        Microsoft::WRL::ComPtr<IDXGIResource1> resource;
        hr = slot->texture.As(&resource);
        if (FAILED(hr) || !resource) return nullptr;
        hr = resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            nullptr, &slot->handle);
        if (FAILED(hr) || !slot->handle) return nullptr;
        slot_ptr = std::move(slot);
    }
    spdlog::info("[D3D11SharedFp16Ring] created generation={} {}x{} buffers={}",
                 generation->id, width, height, kBufferCount);
    return generation;
}

void D3D11SharedFp16Ring::collect_retired_locked() {
    retired_.erase(
        std::remove_if(retired_.begin(), retired_.end(),
            [this](const std::shared_ptr<Generation>& generation) {
                if (generation == latest_generation_) return false;
                return std::none_of(
                    generation->slots.begin(), generation->slots.end(),
                    [](const std::unique_ptr<Slot>& slot) {
                        return slot && (slot->leases != 0 || slot->writing);
                    });
            }),
        retired_.end());
}

} // namespace vr
