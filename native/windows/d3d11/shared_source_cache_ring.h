#pragma once

#include "windows/shared/shared_texture_ring_types.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vr {

class D3D11SharedSourceCacheRing {
public:
    static constexpr int kLiveBufferCount =
        kSharedSourceCacheLiveBufferCount;
    static constexpr uint64_t kDefaultBudgetBytes =
        kSharedSourceCacheDefaultBudgetBytes;

    bool initialize(ID3D11Device* device,
                    ID3D11DeviceContext* context,
                    const std::vector<SourceCacheTrackDescriptor>& descriptors,
                    uint64_t budget_bytes = kDefaultBudgetBytes);
    void clear();
    void shutdown();
    bool reconfigure(
        const std::vector<SourceCacheTrackDescriptor>& descriptors,
        uint64_t budget_bytes = kDefaultBudgetBytes);

    bool begin_bundle(
        std::array<ID3D11RenderTargetView*, 4>& rtvs,
        size_t& texture_count);
    bool publish_bundle(
        std::shared_ptr<const AnalysisOverlayPrimitivePackage> overlay,
        SourceCachePublishInfo* publish_info = nullptr);
    void cancel_bundle();

    bool acquire_latest(SharedSourceCacheBundleSnapshot& snapshot);
    void release(int buffer_index, uint64_t ring_generation);
    void set_frame_callback(std::function<void()> callback);

    uint64_t estimated_bytes() const;
    uint64_t publish_count() const;
    uint64_t backpressure_count() const;
    uint64_t fallback_count() const;
    uint64_t generation() const;
    int ring_depth() const;
    bool frozen_snapshot() const;
    size_t texture_count() const;
    std::string last_error() const;

private:
    struct TrackTexture {
        SourceCacheTrackDescriptor descriptor;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
        HANDLE handle = nullptr;
        ~TrackTexture();
    };

    struct BundleSlot {
        std::vector<std::unique_ptr<TrackTexture>> textures;
        uint32_t leases = 0;
        uint64_t frame_generation = 0;
        bool writing = false;
        std::shared_ptr<const AnalysisOverlayPrimitivePackage> overlay;
    };

    struct Generation {
        uint64_t id = 0;
        int depth = 0;
        uint64_t estimated_bytes = 0;
        bool frozen_snapshot = false;
        std::vector<std::unique_ptr<BundleSlot>> bundles;
    };

    std::shared_ptr<Generation> create_generation(
        const std::vector<SourceCacheTrackDescriptor>& descriptors,
        uint64_t budget_bytes);
    void collect_retired_locked();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    mutable std::mutex mutex_;
    std::shared_ptr<Generation> active_;
    std::vector<std::shared_ptr<Generation>> retired_;
    std::shared_ptr<Generation> latest_generation_;
    BundleSlot* latest_ = nullptr;
    BundleSlot* writing_ = nullptr;
    int writing_index_ = -1;
    uint64_t next_ring_generation_ = 1;
    uint64_t next_frame_generation_ = 1;
    uint64_t publish_count_ = 0;
    uint64_t backpressure_count_ = 0;
    uint64_t fallback_count_ = 0;
    std::string last_error_ = "none";
    std::function<void()> callback_;
};

} // namespace vr
