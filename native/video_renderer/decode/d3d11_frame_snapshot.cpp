#include "video_renderer/decode/d3d11_frame_snapshot.h"
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include "video_renderer/d3d11/memory_estimate.h"
#include <chrono>
#include <d3d11.h>
#include <thread>
#include <utility>
#include <vector>
#include <wrl/client.h>
#endif

namespace vr {

#ifdef _WIN32
namespace {

bool same_snapshot_desc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
    return a.Width == b.Width &&
           a.Height == b.Height &&
           a.MipLevels == b.MipLevels &&
           a.ArraySize == b.ArraySize &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality &&
           a.Usage == b.Usage &&
           a.BindFlags == b.BindFlags &&
           a.CPUAccessFlags == b.CPUAccessFlags &&
           a.MiscFlags == b.MiscFlags;
}

bool d3d11_surface_is_supported_yuv420(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_NV12 ||
           format == DXGI_FORMAT_P010 ||
           format == DXGI_FORMAT_P016;
}

struct D3D11SnapshotFrameRef {
    ~D3D11SnapshotFrameRef();

    std::weak_ptr<D3D11SnapshotPool> pool;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
};

void wait_d3d11_context_idle(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!context) {
        return;
    }
    if (!device) {
        context->Flush();
        return;
    }

    D3D11_QUERY_DESC query_desc = {};
    query_desc.Query = D3D11_QUERY_EVENT;
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    HRESULT hr = device->CreateQuery(&query_desc, &query);
    if (FAILED(hr) || !query) {
        context->Flush();
        return;
    }

    context->End(query.Get());
    context->Flush();
    const auto start = std::chrono::steady_clock::now();
    while ((hr = context->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100)) {
            spdlog::warn("[D3D11FrameSnapshot] Snapshot fence timeout after 100ms");
            break;
        }
    }
    if (FAILED(hr)) {
        spdlog::warn("[D3D11FrameSnapshot] Snapshot fence GetData failed: {:#x}",
                     static_cast<unsigned long>(hr));
    }
}

}  // namespace

struct D3D11SnapshotPool {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> acquire(
        ID3D11Device* device,
        const D3D11_TEXTURE2D_DESC& desc) {
        if (!device) {
            return {};
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = available.begin(); it != available.end(); ++it) {
                if (!*it) {
                    it = available.erase(it);
                    if (it == available.end()) break;
                    continue;
                }
                D3D11_TEXTURE2D_DESC existing_desc = {};
                (*it)->GetDesc(&existing_desc);
                if (!same_snapshot_desc(existing_desc, desc)) {
                    continue;
                }

                Microsoft::WRL::ComPtr<ID3D11Texture2D> texture = *it;
                available.erase(it);
                ++reused_count;
                ++checked_out_count;
                last_desc = desc;
                return texture;
            }
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(hr) || !texture) {
            spdlog::warn("[D3D11FrameSnapshot] Failed to create exact-seek snapshot: {:#x}",
                         static_cast<unsigned long>(hr));
            return {};
        }

        std::lock_guard<std::mutex> lock(mutex);
        ++created_count;
        ++checked_out_count;
        last_desc = desc;
        return texture;
    }

    void release(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture) {
        if (!texture) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (checked_out_count > 0) {
            --checked_out_count;
        }
        if (available.size() >= kMaxAvailable) {
            return;
        }
        available.push_back(std::move(texture));
    }

    D3D11SnapshotPoolStats stats() const {
        std::lock_guard<std::mutex> lock(mutex);
        D3D11SnapshotPoolStats result;
        result.created_count = created_count;
        result.reused_count = reused_count;
        result.checked_out_count = checked_out_count;
        result.available_count = available.size();
        result.width = static_cast<int>(last_desc.Width);
        result.height = static_cast<int>(last_desc.Height);
        result.format = static_cast<int>(last_desc.Format);
        result.texture_bytes = estimate_d3d11_texture_bytes(last_desc);
        result.estimated_bytes = result.texture_bytes *
            static_cast<uint64_t>(checked_out_count + available.size());
        return result;
    }

    static constexpr size_t kMaxAvailable = 1;
    mutable std::mutex mutex;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> available;
    uint64_t created_count = 0;
    uint64_t reused_count = 0;
    size_t checked_out_count = 0;
    D3D11_TEXTURE2D_DESC last_desc = {};
};

namespace {

D3D11SnapshotFrameRef::~D3D11SnapshotFrameRef() {
    if (auto owner = pool.lock()) {
        owner->release(texture);
    }
}

}  // namespace
#endif  // _WIN32

std::shared_ptr<D3D11SnapshotPool> create_d3d11_snapshot_pool() {
#ifdef _WIN32
    return std::make_shared<D3D11SnapshotPool>();
#else
    return nullptr;
#endif
}

bool populate_d3d11_hardware_texture_frame(AVFrame* frame, TextureFrame& result) {
#ifdef _WIN32
    if (!frame || !frame->data[0]) {
        spdlog::error("[D3D11FrameSnapshot] Hardware frame has no D3D11 texture");
        return false;
    }

    result.texture_handle = frame->data[0];
    result.is_ref = true;

    auto* texture = static_cast<ID3D11Texture2D*>(result.texture_handle);
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (!d3d11_surface_is_supported_yuv420(desc.Format)) {
        spdlog::error("[D3D11FrameSnapshot] Unsupported D3D11VA surface format {}. "
                      "Renderer-owned hardware path only supports NV12/P010/P016; "
                      "use software decode until 4:2:2/4:4:4 GPU shader paths exist.",
                      static_cast<int>(desc.Format));
        return false;
    }
    result.is_nv12 = true;
    result.is_p010 = desc.Format == DXGI_FORMAT_P010 ||
        desc.Format == DXGI_FORMAT_P016;
    result.texture_array_index = static_cast<int>(
        reinterpret_cast<intptr_t>(frame->data[1]));

    AVFrame* ref_frame = av_frame_alloc();
    if (ref_frame && av_frame_ref(ref_frame, frame) >= 0) {
        result.hw_frame_ref = std::shared_ptr<void>(ref_frame, [](void* p) {
            AVFrame* f = static_cast<AVFrame*>(p);
            av_frame_free(&f);
        });
    } else {
        spdlog::warn("[D3D11FrameSnapshot] Failed to ref hw frame, texture may be recycled early");
        if (ref_frame) av_frame_free(&ref_frame);
    }

    result.storage = D3D11Nv12FrameStorage{
        static_cast<ID3D11Texture2D*>(result.texture_handle),
        result.texture_array_index,
        result.hw_frame_ref,
    };
    return true;
#else
    (void)frame;
    (void)result;
    spdlog::error("[D3D11FrameSnapshot] Renderer-owned D3D11 frames are Windows-only");
    return false;
#endif
}

std::optional<TextureFrame> snapshot_d3d11_hardware_frame(
    AVFrame* frame,
    const TextureFrame& metadata,
    std::recursive_mutex* device_mutex,
    std::shared_ptr<D3D11SnapshotPool>& snapshot_pool) {
#ifdef _WIN32
    if (!frame || !frame->data[0]) {
        return std::nullopt;
    }

    std::unique_lock<std::recursive_mutex> d3d_lock;
    if (device_mutex) {
        d3d_lock = std::unique_lock<std::recursive_mutex>(*device_mutex);
    }

    auto* source = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const int array_idx = static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));
    if (array_idx < 0) {
        return std::nullopt;
    }

    D3D11_TEXTURE2D_DESC source_desc = {};
    source->GetDesc(&source_desc);
    if (!d3d11_surface_is_supported_yuv420(source_desc.Format)) {
        spdlog::warn("[D3D11FrameSnapshot] Cannot snapshot unsupported D3D11VA surface format {}",
                     static_cast<int>(source_desc.Format));
        return std::nullopt;
    }
    if (static_cast<UINT>(array_idx) >= source_desc.ArraySize) {
        spdlog::warn("[D3D11FrameSnapshot] Snapshot array index out of range: idx={}, array_size={}",
                     array_idx, source_desc.ArraySize);
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    if (!device) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) {
        return std::nullopt;
    }

    D3D11_TEXTURE2D_DESC snapshot_desc = source_desc;
    snapshot_desc.ArraySize = 1;
    snapshot_desc.Usage = D3D11_USAGE_DEFAULT;
    snapshot_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    snapshot_desc.CPUAccessFlags = 0;
    snapshot_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    if (!snapshot_pool) {
        snapshot_pool = std::make_shared<D3D11SnapshotPool>();
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> snapshot =
        snapshot_pool->acquire(device.Get(), snapshot_desc);
    if (!snapshot) {
        return std::nullopt;
    }

    context->CopySubresourceRegion(
        snapshot.Get(),
        0,
        0, 0, 0,
        source,
        D3D11CalcSubresource(0, static_cast<UINT>(array_idx), source_desc.MipLevels),
        nullptr);
    wait_d3d11_context_idle(device.Get(), context.Get());

    auto snapshot_ref = std::make_shared<D3D11SnapshotFrameRef>();
    snapshot_ref->pool = snapshot_pool;
    snapshot_ref->texture = snapshot;

    TextureFrame result = metadata;
    result.is_ref = true;
    result.texture_handle = snapshot.Get();
    result.is_nv12 = true;
    result.is_p010 = source_desc.Format == DXGI_FORMAT_P010 ||
        source_desc.Format == DXGI_FORMAT_P016;
    result.texture_array_index = 0;
    result.hw_frame_ref = snapshot_ref;
    result.storage = D3D11Nv12FrameStorage{
        snapshot.Get(),
        0,
        result.hw_frame_ref,
    };
    return result;
#else
    (void)frame;
    (void)metadata;
    (void)device_mutex;
    (void)snapshot_pool;
    return std::nullopt;
#endif
}

D3D11SnapshotPoolStats d3d11_snapshot_pool_stats(
    const std::shared_ptr<D3D11SnapshotPool>& snapshot_pool) {
#ifdef _WIN32
    return snapshot_pool ? snapshot_pool->stats() : D3D11SnapshotPoolStats{};
#else
    (void)snapshot_pool;
    return {};
#endif
}

} // namespace vr
