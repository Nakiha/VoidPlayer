#include "windows/wgpu/d3d12_presentation_backend.h"

#include <array>
#include <algorithm>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mutex>
#include <spdlog/spdlog.h>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace vr {

class WgpuD3D12SharedFp16Ring {
public:
    static constexpr int kBufferCount = 3;

    ~WgpuD3D12SharedFp16Ring() { shutdown(); }

    bool initialize(ID3D12Device* device, int width, int height) {
        shutdown();
        device_ = device;
        active_ = create_generation(width, height);
        return active_ != nullptr;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = {};
        writing_ = nullptr;
        writing_index_ = -1;
        latest_ = nullptr;
        active_.reset();
        retired_.clear();
        device_ = nullptr;
    }

    bool resize(int width, int height) {
        auto replacement = create_generation(width, height);
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

    ID3D12Resource* begin_frame(int width, int height) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ && active_->width == width && active_->height == height) {
                return begin_frame_locked();
            }
        }
        if (!resize(width, height)) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return begin_frame_locked();
    }

    bool publish_frame() {
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
        if (callback) {
            callback();
        }
        return slot != nullptr;
    }

    void cancel_frame() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!writing_) {
            return;
        }
        writing_->writing = false;
        writing_ = nullptr;
        writing_index_ = -1;
    }

    bool acquire_latest(SharedFp16TextureSnapshot& snapshot) {
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
        snapshot.sync_mode = SharedFp16TextureSyncMode::PublishedAfterProducerWait;
        snapshot.consumer_acquire_key = 0;
        snapshot.producer_release_key = 0;
        for (int i = 0; i < kBufferCount; ++i) {
            if (latest_generation_->slots[i].get() == latest_) {
                snapshot.buffer_index = i;
                break;
            }
        }
        return snapshot.handle != nullptr && snapshot.buffer_index >= 0;
    }

    void release(int buffer_index, uint64_t ring_generation) {
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

    void set_frame_callback(std::function<void()> callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    uint64_t publish_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return publish_count_;
    }

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        HANDLE handle = nullptr;
        uint32_t leases = 0;
        uint64_t frame_generation = 0;
        bool writing = false;
        ~Slot() {
            if (handle) {
                CloseHandle(handle);
            }
        }
    };

    struct Generation {
        uint64_t id = 0;
        int width = 0;
        int height = 0;
        std::array<std::unique_ptr<Slot>, kBufferCount> slots;
    };

    ID3D12Resource* begin_frame_locked() {
        if (!active_ || writing_) {
            return nullptr;
        }
        for (int i = 0; i < kBufferCount; ++i) {
            auto* slot = active_->slots[i].get();
            if (!slot || slot->writing || slot->leases != 0 ||
                (latest_generation_ == active_ && latest_ == slot)) {
                continue;
            }
            slot->writing = true;
            writing_ = slot;
            writing_index_ = i;
            return slot->texture.Get();
        }
        ++backpressure_count_;
        return nullptr;
    }

    std::shared_ptr<Generation> create_generation(int width, int height) {
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
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heap.CreationNodeMask = 1;
            heap.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = static_cast<UINT64>(width);
            desc.Height = static_cast<UINT>(height);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clear = {};
            clear.Format = desc.Format;
            clear.Color[3] = 1.0f;

            HRESULT hr = device_->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_SHARED,
                &desc,
                D3D12_RESOURCE_STATE_COMMON,
                &clear,
                IID_PPV_ARGS(&slot->texture));
            if (FAILED(hr) || !slot->texture) {
                spdlog::error(
                    "[WgpuD3D12SharedFp16Ring] CreateCommittedResource failed: {:#x}",
                    static_cast<unsigned long>(hr));
                return nullptr;
            }
            hr = device_->CreateSharedHandle(
                slot->texture.Get(),
                nullptr,
                GENERIC_ALL,
                nullptr,
                &slot->handle);
            if (FAILED(hr) || !slot->handle) {
                spdlog::error(
                    "[WgpuD3D12SharedFp16Ring] CreateSharedHandle failed: {:#x}",
                    static_cast<unsigned long>(hr));
                return nullptr;
            }
            slot_ptr = std::move(slot);
        }
        spdlog::info(
            "[WgpuD3D12SharedFp16Ring] created generation={} {}x{} buffers={}",
            generation->id,
            width,
            height,
            kBufferCount);
        return generation;
    }

    void collect_retired_locked() {
        retired_.erase(
            std::remove_if(
                retired_.begin(),
                retired_.end(),
                [this](const std::shared_ptr<Generation>& generation) {
                    if (generation == latest_generation_) {
                        return false;
                    }
                    return std::none_of(
                        generation->slots.begin(),
                        generation->slots.end(),
                        [](const std::unique_ptr<Slot>& slot) {
                            return slot && (slot->leases != 0 || slot->writing);
                        });
                }),
            retired_.end());
    }

    ID3D12Device* device_ = nullptr;
    mutable std::mutex mutex_;
    std::shared_ptr<Generation> active_;
    std::shared_ptr<Generation> latest_generation_;
    std::vector<std::shared_ptr<Generation>> retired_;
    Slot* latest_ = nullptr;
    Slot* writing_ = nullptr;
    int writing_index_ = -1;
    uint64_t next_ring_generation_ = 1;
    uint64_t next_frame_generation_ = 1;
    uint64_t publish_count_ = 0;
    uint64_t backpressure_count_ = 0;
    std::function<void()> callback_;
};

WgpuD3D12PresentationBackend::WgpuD3D12PresentationBackend() = default;

WgpuD3D12PresentationBackend::~WgpuD3D12PresentationBackend() {
    shutdown();
}

bool WgpuD3D12PresentationBackend::initialize(
    const PresentationBackendConfig& config) {
    shutdown();
    headless_ = config.headless;
#if VOIDPLAYER_WGPU_RUST_LINKED
    if (VPWgpuFfiVersion() != VP_WGPU_FFI_ABI_VERSION) {
        last_error_ = "wgpu-d3d12 Rust FFI ABI mismatch";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        return false;
    }
    std::array<char, 512> error{};
    renderer_ = VPWgpuD3D12RendererCreate(error.data(), error.size());
    if (!renderer_) {
        last_error_ = error.data()[0] != '\0'
                          ? error.data()
                          : "wgpu-d3d12 renderer creation failed";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        return false;
    }
    if (VPWgpuD3D12RendererGetInfo(renderer_, &renderer_info_) != 0) {
        last_error_ = "wgpu-d3d12 renderer info query failed";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        shutdown();
        return false;
    }
    if (!VPWgpuD3D12RendererD3D12Device(renderer_)) {
        last_error_ = "wgpu-d3d12 renderer did not expose an ID3D12Device";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        shutdown();
        return false;
    }
    last_error_.clear();
    spdlog::info(
        "[WgpuD3D12] initialized adapter='{}' backend='{}' device_type='{}' "
        "nv12={} p010={} rgba16f={}",
        renderer_info_.adapter_description,
        renderer_info_.backend,
        renderer_info_.device_type,
        renderer_info_.supports_nv12 != 0,
        renderer_info_.supports_p010 != 0,
        renderer_info_.supports_rgba16_float != 0);
    if (config.shared_fp16_output) {
        auto* d3d12_device = static_cast<ID3D12Device*>(
            VPWgpuD3D12RendererD3D12Device(renderer_));
        shared_fp16_ring_ = std::make_unique<WgpuD3D12SharedFp16Ring>();
        if (!shared_fp16_ring_->initialize(
                d3d12_device,
                std::max(config.width, 1),
                std::max(config.height, 1))) {
            last_error_ = "wgpu-d3d12 shared FP16 output initialization failed";
            spdlog::error("[WgpuD3D12] {}", last_error_);
            shared_fp16_ring_.reset();
            shutdown();
            return false;
        }
        shared_fp16_ring_->set_frame_callback(shared_fp16_callback_);
    }
    return true;
#else
    last_error_ =
        "wgpu-d3d12 Rust FFI is not linked; D3D11 fallback is disabled";
    spdlog::error("[WgpuD3D12] {}", last_error_);
    return false;
#endif
}

void WgpuD3D12PresentationBackend::shutdown() {
    if (shared_fp16_ring_) {
        shared_fp16_ring_->shutdown();
        shared_fp16_ring_.reset();
    }
    if (renderer_) {
        VPWgpuD3D12RendererDestroy(renderer_);
        renderer_ = nullptr;
    }
    renderer_info_ = VPWgpuD3D12RendererInfo{};
    headless_ = false;
}

void* WgpuD3D12PresentationBackend::native_render_device() const {
#if VOIDPLAYER_WGPU_RUST_LINKED
    return renderer_ ? VPWgpuD3D12RendererD3D12Device(renderer_) : nullptr;
#else
    return nullptr;
#endif
}

bool WgpuD3D12PresentationBackend::acquire_shared_fp16_texture(
    SharedFp16TextureSnapshot& snapshot) {
    return shared_fp16_ring_ && shared_fp16_ring_->acquire_latest(snapshot);
}

void WgpuD3D12PresentationBackend::release_shared_fp16_texture(
    int buffer_index,
    uint64_t ring_generation) {
    if (shared_fp16_ring_) {
        shared_fp16_ring_->release(buffer_index, ring_generation);
    }
}

void WgpuD3D12PresentationBackend::set_shared_fp16_frame_callback(
    std::function<void()> callback) {
    shared_fp16_callback_ = std::move(callback);
    if (shared_fp16_ring_) {
        shared_fp16_ring_->set_frame_callback(shared_fp16_callback_);
    }
}

PresentationBackendDiagnostics WgpuD3D12PresentationBackend::diagnostics() const {
    PresentationBackendDiagnostics diagnostics;
    diagnostics.backend = "wgpu-d3d12";
    diagnostics.fallback_reason = last_error_.empty() ? "none" : last_error_;
    diagnostics.target_format = "R16G16B16A16_FLOAT";
    diagnostics.render_target_format = "wgpu-d3d12";
    diagnostics.render_color_space = "unknown";
    diagnostics.headless = headless_;
    diagnostics.adapter_description = renderer_info_.adapter_description;
    diagnostics.driver_type = renderer_info_.driver_type;
    diagnostics.adapter_vendor_id = static_cast<int32_t>(renderer_info_.vendor_id);
    diagnostics.adapter_device_id = static_cast<int32_t>(renderer_info_.device_id);
    return diagnostics;
}

bool WgpuD3D12PresentationBackend::draw_frame(
    const RendererDrawSnapshot& snapshot,
    const PresentationBackendDrawHooks& hooks) {
    (void)hooks;
#if VOIDPLAYER_WGPU_RUST_LINKED
    if (!renderer_ || !shared_fp16_ring_) {
        last_error_ =
            "wgpu-d3d12 draw requires renderer and shared FP16 output";
        return false;
    }
    ID3D12Resource* target = shared_fp16_ring_->begin_frame(
        std::max(snapshot.target_width, 1),
        std::max(snapshot.target_height, 1));
    if (!target) {
        last_error_ = "wgpu-d3d12 shared FP16 output has no free buffer";
        return false;
    }
    std::array<char, 512> error{};
    VPWgpuD3D12RenderTargetClearRequest request = {};
    request.d3d12_resource = target;
    request.format = VP_WGPU_D3D12_TEXTURE_FORMAT_RGBA16_FLOAT;
    request.width = static_cast<uint32_t>(std::max(snapshot.target_width, 1));
    request.height = static_cast<uint32_t>(std::max(snapshot.target_height, 1));
    request.color[0] = snapshot.background_color[0];
    request.color[1] = snapshot.background_color[1];
    request.color[2] = snapshot.background_color[2];
    request.color[3] = snapshot.background_color[3];
    request.error = error.data();
    request.error_size = error.size();
    if (VPWgpuD3D12RendererClearRenderTargetForProbe(renderer_, &request) != 0) {
        shared_fp16_ring_->cancel_frame();
        last_error_ = error.data()[0] != '\0'
                          ? error.data()
                          : "wgpu-d3d12 shared FP16 clear failed";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        return false;
    }
    if (!shared_fp16_ring_->publish_frame()) {
        last_error_ = "wgpu-d3d12 shared FP16 publish failed";
        return false;
    }
    last_error_.clear();
    return true;
#else
    (void)snapshot;
    last_error_ =
        "wgpu-d3d12 Rust FFI is not linked; D3D11 fallback is disabled";
    return false;
#endif
}

} // namespace vr
