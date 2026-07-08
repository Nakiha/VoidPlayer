#pragma once

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace vr {

struct D3D11HeadlessOutputMemoryStats {
    uint64_t estimated_bytes = 0;
    uint64_t texture_bytes = 0;
    int width = 0;
    int height = 0;
    int format = 0;
    int buffer_count = 0;
};

struct D3D11HeadlessOutputTextureLease {
    ID3D11Texture2D* texture = nullptr;  // AddRef'd; caller must Release().
    HANDLE handle = nullptr;
    int width = 0;
    int height = 0;
    int buffer_index = -1;
    uint64_t generation = 0;
};

struct D3D11HeadlessOutputFrontBufferSnapshot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    int width = 0;
    int height = 0;
};

class D3D11HeadlessOutput {
public:
    static constexpr int kBufferCount = 3;

    D3D11HeadlessOutput() = default;
    ~D3D11HeadlessOutput() = default;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);
    void shutdown();
    void fail_shared_handle_for_test(bool enabled);

    // Methods with the _locked suffix require callers to hold texture_mutex().
    // Renderer keeps lock ordering as device_mutex -> texture_mutex.
    ID3D11Texture2D* shared_texture_locked() const;
    HANDLE shared_texture_handle_locked() const;
    std::mutex& texture_mutex() const { return texture_mutex_; }
    bool acquire_shared_texture_locked(D3D11HeadlessOutputTextureLease& lease);
    void release_shared_texture(int buffer_index, uint64_t generation);
    bool buffer_in_flight_for_test(int buffer_index) const;

    ID3D11RenderTargetView* begin_frame_locked();
    std::function<void()> publish_frame_locked();
    void wait_gpu_idle(const char* label);

    bool resize_locked(int width, int height);
    void cleanup_expired_pending_buffers();
    bool snapshot_front_buffer_locked(D3D11HeadlessOutputFrontBufferSnapshot& snapshot) const;
    // Caller serializes D3D11 immediate-context access; texture_mutex() is not needed.
    bool capture_front_buffer_snapshot(const D3D11HeadlessOutputFrontBufferSnapshot& snapshot,
                                       std::vector<uint8_t>& bgra,
                                       int& width,
                                       int& height);
    bool capture_front_buffer_region_snapshot(
        const D3D11HeadlessOutputFrontBufferSnapshot& snapshot,
        int x,
        int y,
        int width,
        int height,
        std::vector<uint8_t>& bgra,
        int& region_width,
        int& region_height);

    void set_frame_callback(std::function<void()> cb);
    D3D11HeadlessOutputMemoryStats memory_stats() const;

private:
    struct SharedBuffers {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[kBufferCount];
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[kBufferCount];
        HANDLE handles[kBufferCount] = {};
        std::atomic<int> front{0};
        uint32_t in_flight_count[kBufferCount] = {};
        uint64_t generation = 1;
    };

    bool create_shared_buffers(int width,
                               int height,
                               Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[],
                               Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[],
                               HANDLE handles[]);
    int pick_free_buffer_locked() const;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    SharedBuffers buffers_;
    Microsoft::WRL::ComPtr<ID3D11Query> gpu_fence_;
    mutable std::mutex texture_mutex_;
    std::function<void()> frame_callback_;
    int current_back_ = -1;
    bool fail_shared_handle_for_test_ = false;
};

} // namespace vr
