#include "windows/d3d11/shared_source_cache_ring.h"

#include "windows/d3d11/memory_estimate.h"

#include <algorithm>
#include <dxgi1_2.h>
#include <limits>
#include <spdlog/spdlog.h>

namespace vr {
namespace {

bool valid_descriptor(const SourceCacheTrackDescriptor& descriptor) {
    return descriptor.slot >= 0 && descriptor.slot < 4 &&
           descriptor.file_id >= 0 &&
           descriptor.width > 0 && descriptor.height > 0;
}

} // namespace

SourceCacheRingPolicy resolve_source_cache_ring_policy(
    const std::vector<SourceCacheTrackDescriptor>& descriptors,
    uint64_t budget_bytes) {
    SourceCacheRingPolicy result;
    if (descriptors.empty() || descriptors.size() > 4 || budget_bytes == 0) {
        return result;
    }
    uint64_t bytes_per_frame = 0;
    std::array<bool, 4> slots{};
    for (const auto& descriptor : descriptors) {
        if (!valid_descriptor(descriptor) || slots[descriptor.slot]) {
            return result;
        }
        slots[descriptor.slot] = true;
        const uint64_t bytes = estimate_dxgi_surface_bytes(
            static_cast<UINT>(descriptor.width),
            static_cast<UINT>(descriptor.height),
            DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (bytes == 0 ||
            bytes_per_frame > std::numeric_limits<uint64_t>::max() - bytes) {
            return result;
        }
        bytes_per_frame += bytes;
    }
    result.bytes_per_frame = bytes_per_frame;
    result.depth =
        bytes_per_frame <=
                budget_bytes / D3D11SharedSourceCacheRing::kLiveBufferCount
            ? D3D11SharedSourceCacheRing::kLiveBufferCount
            : 1;
    result.total_bytes = bytes_per_frame * static_cast<uint64_t>(result.depth);
    result.frozen_snapshot = result.depth == 1;
    result.allowed = result.total_bytes <= budget_bytes;
    return result;
}

D3D11SharedSourceCacheRing::TrackTexture::~TrackTexture() {
    if (handle) {
        CloseHandle(handle);
    }
}

bool D3D11SharedSourceCacheRing::initialize(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    const std::vector<SourceCacheTrackDescriptor>& descriptors,
    uint64_t budget_bytes) {
    shutdown();
    device_ = device;
    context_ = context;
    active_ = create_generation(descriptors, budget_bytes);
    return active_ != nullptr;
}

void D3D11SharedSourceCacheRing::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (writing_) {
        writing_->writing = false;
        for (const auto& texture : writing_->textures) {
            texture->keyed_mutex->ReleaseSync(0);
        }
    }
    writing_ = nullptr;
    writing_index_ = -1;
    if (active_) {
        retired_.push_back(active_);
    }
    active_.reset();
    latest_ = nullptr;
    latest_generation_.reset();
    collect_retired_locked();
}

void D3D11SharedSourceCacheRing::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = {};
    writing_ = nullptr;
    latest_ = nullptr;
    active_.reset();
    latest_generation_.reset();
    retired_.clear();
    device_ = nullptr;
    context_ = nullptr;
}

bool D3D11SharedSourceCacheRing::reconfigure(
    const std::vector<SourceCacheTrackDescriptor>& descriptors,
    uint64_t budget_bytes) {
    auto replacement = create_generation(descriptors, budget_bytes);
    if (!replacement) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++fallback_count_;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
        retired_.push_back(active_);
    }
    active_ = std::move(replacement);
    latest_ = nullptr;
    latest_generation_.reset();
    writing_ = nullptr;
    writing_index_ = -1;
    collect_retired_locked();
    return true;
}

bool D3D11SharedSourceCacheRing::begin_bundle(
    std::array<ID3D11RenderTargetView*, 4>& rtvs,
    size_t& texture_count) {
    rtvs = {};
    texture_count = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || writing_) {
        return false;
    }
    if (active_->frozen_snapshot && latest_generation_ == active_ && latest_) {
        return false;
    }
    for (int i = 0; i < active_->depth; ++i) {
        auto* bundle = active_->bundles[static_cast<size_t>(i)].get();
        if (!bundle || bundle->writing || bundle->leases != 0 ||
            (latest_generation_ == active_ && latest_ == bundle)) {
            continue;
        }
        size_t acquired = 0;
        for (; acquired < bundle->textures.size(); ++acquired) {
            auto* texture = bundle->textures[acquired].get();
            if (!texture || !texture->keyed_mutex ||
                texture->keyed_mutex->AcquireSync(0, 0) != S_OK) {
                break;
            }
        }
        if (acquired != bundle->textures.size()) {
            while (acquired > 0) {
                --acquired;
                bundle->textures[acquired]->keyed_mutex->ReleaseSync(0);
            }
            continue;
        }
        bundle->writing = true;
        writing_ = bundle;
        writing_index_ = i;
        texture_count = bundle->textures.size();
        for (size_t track = 0; track < texture_count; ++track) {
            rtvs[track] = bundle->textures[track]->rtv.Get();
        }
        return true;
    }
    ++backpressure_count_;
    return false;
}

bool D3D11SharedSourceCacheRing::publish_bundle(
    std::shared_ptr<const AnalysisOverlayPrimitivePackage> overlay) {
    std::function<void()> callback;
    BundleSlot* bundle = nullptr;
    uint64_t ring_generation = 0;
    uint64_t frame_generation = 0;
    size_t texture_count = 0;
    bool first_publish = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!writing_ || !active_) {
            return false;
        }
        bundle = writing_;
        bundle->writing = false;
        bundle->frame_generation = next_frame_generation_++;
        bundle->overlay = std::move(overlay);
        latest_ = bundle;
        latest_generation_ = active_;
        writing_ = nullptr;
        writing_index_ = -1;
        ++publish_count_;
        ring_generation = active_->id;
        frame_generation = bundle->frame_generation;
        texture_count = bundle->textures.size();
        first_publish = publish_count_ == 1;
        callback = callback_;
        last_error_ = "none";
        collect_retired_locked();
    }
    bool released = true;
    for (const auto& texture : bundle->textures) {
        released =
            SUCCEEDED(texture->keyed_mutex->ReleaseSync(1)) && released;
    }
    if (callback) {
        callback();
    }
    if (first_publish) {
        spdlog::info(
            "[D3D11SourceCache] first publish ring_generation={} "
            "frame_generation={} textures={}",
            ring_generation,
            frame_generation,
            texture_count);
    }
    return released;
}

void D3D11SharedSourceCacheRing::cancel_bundle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!writing_) {
        return;
    }
    writing_->writing = false;
    for (const auto& texture : writing_->textures) {
        texture->keyed_mutex->ReleaseSync(0);
    }
    writing_ = nullptr;
    writing_index_ = -1;
}

bool D3D11SharedSourceCacheRing::acquire_latest(
    SharedSourceCacheBundleSnapshot& snapshot) {
    snapshot = {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_ || !latest_generation_ || latest_->writing) {
        return false;
    }
    ++latest_->leases;
    snapshot.ring_generation = latest_generation_->id;
    snapshot.frame_generation = latest_->frame_generation;
    snapshot.ring_depth = latest_generation_->depth;
    snapshot.overlay = latest_->overlay;
    for (size_t i = 0; i < latest_generation_->bundles.size(); ++i) {
        if (latest_generation_->bundles[i].get() == latest_) {
            snapshot.buffer_index = static_cast<int>(i);
            break;
        }
    }
    snapshot.texture_count = latest_->textures.size();
    for (size_t i = 0; i < snapshot.texture_count; ++i) {
        const auto& texture = *latest_->textures[i];
        auto& out = snapshot.textures[i];
        out.handle = texture.handle;
        out.source_slot = texture.descriptor.slot;
        out.source_file_id = texture.descriptor.file_id;
        out.width = texture.descriptor.width;
        out.height = texture.descriptor.height;
        out.color_transfer = texture.descriptor.color_transfer;
    }
    return snapshot.buffer_index >= 0 && snapshot.texture_count > 0;
}

void D3D11SharedSourceCacheRing::release(
    int buffer_index, uint64_t ring_generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto release_from = [&](const std::shared_ptr<Generation>& generation) {
        if (!generation || generation->id != ring_generation ||
            buffer_index < 0 ||
            buffer_index >= static_cast<int>(generation->bundles.size())) {
            return false;
        }
        auto* bundle = generation->bundles[static_cast<size_t>(buffer_index)].get();
        if (bundle && bundle->leases > 0) {
            --bundle->leases;
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

void D3D11SharedSourceCacheRing::set_frame_callback(
    std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

uint64_t D3D11SharedSourceCacheRing::estimated_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t bytes = active_ ? active_->estimated_bytes : 0;
    for (const auto& generation : retired_) {
        if (generation) {
            bytes += generation->estimated_bytes;
        }
    }
    return bytes;
}

uint64_t D3D11SharedSourceCacheRing::publish_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return publish_count_;
}

uint64_t D3D11SharedSourceCacheRing::backpressure_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backpressure_count_;
}

uint64_t D3D11SharedSourceCacheRing::fallback_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fallback_count_;
}

uint64_t D3D11SharedSourceCacheRing::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ ? active_->id : 0;
}

int D3D11SharedSourceCacheRing::ring_depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ ? active_->depth : 0;
}

bool D3D11SharedSourceCacheRing::frozen_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && active_->frozen_snapshot;
}

size_t D3D11SharedSourceCacheRing::texture_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || active_->bundles.empty()) {
        return 0;
    }
    return active_->bundles.front()->textures.size();
}

std::string D3D11SharedSourceCacheRing::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::shared_ptr<D3D11SharedSourceCacheRing::Generation>
D3D11SharedSourceCacheRing::create_generation(
    const std::vector<SourceCacheTrackDescriptor>& descriptors,
    uint64_t budget_bytes) {
    if (!device_ || !context_) {
        last_error_ = "source-cache-device-unavailable";
        return nullptr;
    }
    const auto policy =
        resolve_source_cache_ring_policy(descriptors, budget_bytes);
    if (!policy.allowed) {
        last_error_ = "source-cache-memory-cap-exceeded";
        return nullptr;
    }
    auto generation = std::make_shared<Generation>();
    generation->id = next_ring_generation_++;
    generation->depth = policy.depth;
    generation->estimated_bytes = policy.total_bytes;
    generation->frozen_snapshot = policy.frozen_snapshot;
    for (int index = 0; index < generation->depth; ++index) {
        auto bundle = std::make_unique<BundleSlot>();
        for (const auto& descriptor : descriptors) {
            auto track = std::make_unique<TrackTexture>();
            track->descriptor = descriptor;
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = static_cast<UINT>(descriptor.width);
            desc.Height = static_cast<UINT>(descriptor.height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags =
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                             D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            HRESULT hr =
                device_->CreateTexture2D(&desc, nullptr, &track->texture);
            if (FAILED(hr) || !track->texture) {
                last_error_ = "source-cache-texture-allocation-failed";
                return nullptr;
            }
            hr = device_->CreateRenderTargetView(
                track->texture.Get(), nullptr, &track->rtv);
            if (FAILED(hr) || !track->rtv ||
                FAILED(track->texture.As(&track->keyed_mutex)) ||
                !track->keyed_mutex) {
                last_error_ = "source-cache-view-or-mutex-creation-failed";
                return nullptr;
            }
            Microsoft::WRL::ComPtr<IDXGIResource1> resource;
            if (FAILED(track->texture.As(&resource)) || !resource ||
                FAILED(resource->CreateSharedHandle(
                    nullptr,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr,
                    &track->handle)) ||
                !track->handle) {
                last_error_ = "source-cache-shared-handle-creation-failed";
                return nullptr;
            }
            bundle->textures.push_back(std::move(track));
        }
        generation->bundles.push_back(std::move(bundle));
    }
    last_error_ = "none";
    spdlog::info(
        "[D3D11SourceCache] created generation={} tracks={} depth={} "
        "bytes={} frozen={}",
        generation->id,
        descriptors.size(),
        generation->depth,
        generation->estimated_bytes,
        generation->frozen_snapshot);
    return generation;
}

void D3D11SharedSourceCacheRing::collect_retired_locked() {
    retired_.erase(
        std::remove_if(
            retired_.begin(),
            retired_.end(),
            [this](const std::shared_ptr<Generation>& generation) {
                if (!generation || generation == latest_generation_) {
                    return false;
                }
                return std::none_of(
                    generation->bundles.begin(),
                    generation->bundles.end(),
                    [](const std::unique_ptr<BundleSlot>& bundle) {
                        return bundle &&
                               (bundle->leases != 0 || bundle->writing);
                    });
            }),
        retired_.end());
}

} // namespace vr
