#pragma once

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

namespace vr {

class D3D11HeadlessOutput {
public:
    static constexpr int kBufferCount = 3;

    D3D11HeadlessOutput() = default;
    ~D3D11HeadlessOutput() = default;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);
    void shutdown();

    ID3D11Texture2D* shared_texture() const;
    HANDLE shared_texture_handle() const;
    std::mutex& texture_mutex() { return texture_mutex_; }

    ID3D11RenderTargetView* begin_frame();
    void publish_frame(const char* label);
    void wait_gpu_idle(const char* label);

    bool resize(int width, int height);
    void cleanup_expired_pending_buffers();
    bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height);

    void set_frame_callback(std::function<void()> cb);

private:
    struct SharedBuffers {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[kBufferCount];
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[kBufferCount];
        HANDLE handles[kBufferCount] = {};
        std::atomic<int> front{0};
    };

    struct PendingBuffers {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[kBufferCount];
        HANDLE handles[kBufferCount] = {};
        std::chrono::steady_clock::time_point expire_time;
    };

    bool create_shared_buffers(int width,
                               int height,
                               Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[],
                               Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[],
                               HANDLE handles[]);
    int pick_free_buffer() const;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    SharedBuffers buffers_;
    Microsoft::WRL::ComPtr<ID3D11Query> gpu_fence_;
    std::vector<PendingBuffers> pending_destroy_;
    std::atomic<bool> has_pending_destroy_{false};
    std::mutex texture_mutex_;
    std::function<void()> frame_callback_;
    int current_back_ = 0;
};

} // namespace vr
