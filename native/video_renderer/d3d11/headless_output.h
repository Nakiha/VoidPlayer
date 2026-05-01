#pragma once

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>
#include <atomic>
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

    // Methods with the _locked suffix require callers to hold texture_mutex().
    // Renderer keeps lock ordering as device_mutex -> texture_mutex.
    ID3D11Texture2D* shared_texture_locked() const;
    HANDLE shared_texture_handle_locked() const;
    std::mutex& texture_mutex() const { return texture_mutex_; }

    ID3D11RenderTargetView* begin_frame_locked();
    void publish_frame_locked(const char* label);
    void wait_gpu_idle(const char* label);

    bool resize_locked(int width, int height);
    void cleanup_expired_pending_buffers();
    bool capture_front_buffer_locked(std::vector<uint8_t>& bgra, int& width, int& height);

    void set_frame_callback(std::function<void()> cb);

private:
    struct SharedBuffers {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[kBufferCount];
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs[kBufferCount];
        HANDLE handles[kBufferCount] = {};
        std::atomic<int> front{0};
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
    mutable std::mutex texture_mutex_;
    std::function<void()> frame_callback_;
    int current_back_ = 0;
};

} // namespace vr
