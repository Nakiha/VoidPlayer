#pragma once

#include "windows/shared/shared_texture_ring_types.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace vr {

using D3D11SharedFp16RingPrewarmStats = SharedFp16RingPrewarmStats;

class D3D11SharedFp16Ring {
public:
    static constexpr int kBufferCount = kSharedFp16TextureRingBufferCount;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    int width, int height);
    void shutdown();
    bool prewarm(int width, int height);
    bool resize(int width, int height);

    ID3D11RenderTargetView* begin_frame();
    bool publish_frame();
    void cancel_frame();

    bool acquire_latest(SharedFp16TextureSnapshot& snapshot);
    void release(int buffer_index, uint64_t ring_generation);
    void set_frame_callback(std::function<void()> callback);

    uint64_t estimated_bytes() const;
    uint64_t publish_count() const;
    uint64_t backpressure_count() const;
    D3D11SharedFp16RingPrewarmStats prewarm_stats() const;
    int width() const;
    int height() const;

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
        HANDLE handle = nullptr;
        uint32_t leases = 0;
        uint64_t frame_generation = 0;
        bool writing = false;
        ~Slot();
    };

    struct Generation {
        uint64_t id = 0;
        int width = 0;
        int height = 0;
        std::array<std::unique_ptr<Slot>, kBufferCount> slots;
    };

    std::shared_ptr<Generation> create_generation(int width, int height);
    void collect_retired_locked();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    mutable std::mutex mutex_;
    std::shared_ptr<Generation> active_;
    std::shared_ptr<Generation> prewarmed_;
    std::vector<std::shared_ptr<Generation>> retired_;
    std::shared_ptr<Generation> latest_generation_;
    Slot* latest_ = nullptr;
    Slot* writing_ = nullptr;
    int writing_index_ = -1;
    uint64_t next_ring_generation_ = 1;
    uint64_t next_frame_generation_ = 1;
    uint64_t publish_count_ = 0;
    uint64_t backpressure_count_ = 0;
    uint64_t prewarm_request_count_ = 0;
    uint64_t prewarm_ready_count_ = 0;
    uint64_t prewarm_hit_count_ = 0;
    uint64_t prewarm_dropped_count_ = 0;
    uint64_t prewarm_consumed_count_ = 0;
    std::function<void()> callback_;
};

} // namespace vr
