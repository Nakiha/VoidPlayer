#include "windows/decode/d3d11_frame_snapshot.h"

#include "renderer/decode/av_frame_lifetime.h"

#include <spdlog/spdlog.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace vr {
namespace {

bool supported_yuv420_surface(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_NV12 ||
           format == DXGI_FORMAT_P010 ||
           format == DXGI_FORMAT_P016;
}

bool p010_compatible_surface(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_P010 || format == DXGI_FORMAT_P016;
}

bool same_texture_desc(
    const D3D11_TEXTURE2D_DESC& left,
    const D3D11_TEXTURE2D_DESC& right) {
    return left.Width == right.Width &&
           left.Height == right.Height &&
           left.MipLevels == right.MipLevels &&
           left.ArraySize == right.ArraySize &&
           left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count &&
           left.SampleDesc.Quality == right.SampleDesc.Quality &&
           left.Usage == right.Usage &&
           left.BindFlags == right.BindFlags &&
           left.CPUAccessFlags == right.CPUAccessFlags &&
           left.MiscFlags == right.MiscFlags;
}

uint64_t estimate_texture_bytes(const D3D11_TEXTURE2D_DESC& desc) {
    const uint64_t pixels = static_cast<uint64_t>(desc.Width) *
        static_cast<uint64_t>(desc.Height) *
        static_cast<uint64_t>(desc.ArraySize);
    switch (desc.Format) {
    case DXGI_FORMAT_NV12:
        return pixels * 3 / 2;
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
        return pixels * 3;
    default:
        return 0;
    }
}

struct D3D11SnapshotFrameRef {
    ~D3D11SnapshotFrameRef();

    std::weak_ptr<D3D11SnapshotPool> pool;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
};

} // namespace

struct D3D11SnapshotPool {
    bool wait_for_copy_completion(
        ID3D11Device* device,
        ID3D11DeviceContext* context) {
        if (!device || !context) {
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Query> query;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (completion_query_device.Get() != device) {
                completion_query.Reset();
                completion_query_device.Reset();
            }
            if (!completion_query) {
                D3D11_QUERY_DESC query_desc = {};
                query_desc.Query = D3D11_QUERY_EVENT;
                const HRESULT status =
                    device->CreateQuery(&query_desc, &completion_query);
                if (FAILED(status) || !completion_query) {
                    spdlog::warn(
                        "[D3D11FrameSnapshot] Could not create reusable GPU "
                        "completion query: {:#x}",
                        static_cast<unsigned long>(status));
                    return false;
                }
                completion_query_device = device;
            }
            query = completion_query;
        }

        // Flush only submits a shared-resource copy; it may return before the
        // GPU has written the NV12/P010 chroma plane. Keep readiness on the
        // decode thread so presentation can continue to reproject its last
        // complete cached frame while this copy is pending.
        context->End(query.Get());
        context->Flush();
        const auto start = std::chrono::steady_clock::now();
        bool warned = false;
        HRESULT status = S_FALSE;
        while ((status = context->GetData(
                    query.Get(),
                    nullptr,
                    0,
                    D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (!warned && elapsed >= std::chrono::milliseconds(100)) {
                warned = true;
                spdlog::warn(
                    "[D3D11FrameSnapshot] GPU snapshot copy is still pending "
                    "after 100ms; retaining the previous complete presentation "
                    "frame");
            }
            if (elapsed >= std::chrono::seconds(5)) {
                std::lock_guard<std::mutex> lock(mutex);
                ++completion_wait_timeout_count;
                spdlog::error(
                    "[D3D11FrameSnapshot] GPU snapshot copy did not complete "
                    "within 5s");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        const auto elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++completion_wait_count;
            completion_wait_total_us += elapsed_us;
            completion_wait_max_us =
                std::max(completion_wait_max_us, elapsed_us);
            if (elapsed_us > kInteractionFrameBudgetUs) {
                ++completion_wait_over_budget_count;
            }
        }
        if (FAILED(status)) {
            spdlog::warn(
                "[D3D11FrameSnapshot] GPU copy completion query failed: {:#x}",
                static_cast<unsigned long>(status));
            return false;
        }
        return true;
    }

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
                    continue;
                }
                D3D11_TEXTURE2D_DESC existing_desc = {};
                (*it)->GetDesc(&existing_desc);
                if (!same_texture_desc(existing_desc, desc)) {
                    continue;
                }
                auto texture = *it;
                available.erase(it);
                ++reused_count;
                ++checked_out_count;
                last_desc = desc;
                return texture;
            }
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        const HRESULT status = device->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(status) || !texture) {
            spdlog::warn(
                "[D3D11FrameSnapshot] Shared snapshot allocation failed: {:#x}",
                static_cast<unsigned long>(status));
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
        if (available.size() < kMaxAvailable) {
            available.push_back(std::move(texture));
        }
    }

    void discard() {
        std::lock_guard<std::mutex> lock(mutex);
        if (checked_out_count > 0) {
            --checked_out_count;
        }
    }

    D3D11SnapshotPoolStats stats() const {
        std::lock_guard<std::mutex> lock(mutex);
        D3D11SnapshotPoolStats result;
        result.texture_bytes = estimate_texture_bytes(last_desc);
        result.created_count = created_count;
        result.reused_count = reused_count;
        result.completion_wait_count = completion_wait_count;
        result.completion_wait_total_us = completion_wait_total_us;
        result.completion_wait_max_us = completion_wait_max_us;
        result.completion_wait_over_budget_count =
            completion_wait_over_budget_count;
        result.completion_wait_timeout_count = completion_wait_timeout_count;
        result.checked_out_count = checked_out_count;
        result.available_count = available.size();
        result.width = static_cast<int>(last_desc.Width);
        result.height = static_cast<int>(last_desc.Height);
        result.format = static_cast<int>(last_desc.Format);
        result.estimated_bytes = result.texture_bytes *
            static_cast<uint64_t>(checked_out_count + available.size());
        return result;
    }

    static constexpr size_t kMaxAvailable = 1;
    static constexpr uint64_t kInteractionFrameBudgetUs = 8333;
    mutable std::mutex mutex;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> available;
    Microsoft::WRL::ComPtr<ID3D11Device> completion_query_device;
    Microsoft::WRL::ComPtr<ID3D11Query> completion_query;
    uint64_t created_count = 0;
    uint64_t reused_count = 0;
    uint64_t completion_wait_count = 0;
    uint64_t completion_wait_total_us = 0;
    uint64_t completion_wait_max_us = 0;
    uint64_t completion_wait_over_budget_count = 0;
    uint64_t completion_wait_timeout_count = 0;
    size_t checked_out_count = 0;
    D3D11_TEXTURE2D_DESC last_desc = {};
};

namespace {

D3D11SnapshotFrameRef::~D3D11SnapshotFrameRef() {
    if (auto owner = pool.lock()) {
        owner->release(std::move(texture));
    }
}

} // namespace

std::shared_ptr<D3D11SnapshotPool> create_d3d11_snapshot_pool() {
    return std::make_shared<D3D11SnapshotPool>();
}

bool populate_d3d11_hardware_texture_frame(
    AVFrame* frame,
    TextureFrame& result) {
    if (!frame || !frame->data[0]) {
        spdlog::error("[D3D11FrameSnapshot] Hardware frame has no D3D11 texture");
        return false;
    }

    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (!supported_yuv420_surface(desc.Format)) {
        spdlog::error(
            "[D3D11FrameSnapshot] Unsupported hardware surface format {}",
            static_cast<int>(desc.Format));
        return false;
    }

    auto frame_owner = AvFrameOwner::allocate();
    if (!frame_owner || av_frame_ref(frame_owner.get(), frame) < 0) {
        spdlog::error("[D3D11FrameSnapshot] Failed to retain hardware frame");
        return false;
    }

    const int array_index = static_cast<int>(
        reinterpret_cast<intptr_t>(frame->data[1]));
    auto frame_ref = std::shared_ptr<void>(frame_owner.release(), [](void* pointer) {
        auto* owned_frame = static_cast<AVFrame*>(pointer);
        av_frame_free(&owned_frame);
    });

    result.texture_handle = texture;
    result.is_ref = true;
    result.is_nv12 = true;
    result.is_p010 = p010_compatible_surface(desc.Format);
    result.texture_array_index = array_index;
    result.hw_frame_ref = frame_ref;
    result.storage = WindowsD3D11FrameStorage{
        texture,
        array_index,
        result.is_p010,
        static_cast<int>(desc.Width),
        static_cast<int>(desc.Height),
        frame_ref,
    };
    return true;
}

std::optional<TextureFrame> snapshot_d3d11_hardware_frame(
    AVFrame* frame,
    const TextureFrame& metadata,
    std::recursive_mutex* device_mutex,
    std::shared_ptr<D3D11SnapshotPool>& snapshot_pool) {
    if (!frame || !frame->data[0]) {
        return std::nullopt;
    }

    std::unique_lock<std::recursive_mutex> device_lock;
    if (device_mutex) {
        device_lock = std::unique_lock<std::recursive_mutex>(*device_mutex);
    }

    auto* source = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const int array_index = static_cast<int>(
        reinterpret_cast<intptr_t>(frame->data[1]));
    D3D11_TEXTURE2D_DESC source_desc = {};
    source->GetDesc(&source_desc);
    if (!supported_yuv420_surface(source_desc.Format) ||
        array_index < 0 ||
        static_cast<UINT>(array_index) >= source_desc.ArraySize) {
        spdlog::warn(
            "[D3D11FrameSnapshot] Invalid source format or array index (format={}, index={}, size={})",
            static_cast<int>(source_desc.Format),
            array_index,
            source_desc.ArraySize);
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if (device) {
        device->GetImmediateContext(&context);
    }
    if (!device || !context) {
        return std::nullopt;
    }

    D3D11_TEXTURE2D_DESC snapshot_desc = source_desc;
    snapshot_desc.ArraySize = 1;
    snapshot_desc.Usage = D3D11_USAGE_DEFAULT;
    snapshot_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    snapshot_desc.CPUAccessFlags = 0;
    snapshot_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    if (!snapshot_pool) {
        snapshot_pool = create_d3d11_snapshot_pool();
    }
    auto snapshot = snapshot_pool->acquire(device.Get(), snapshot_desc);
    if (!snapshot) {
        return std::nullopt;
    }

    context->CopySubresourceRegion(
        snapshot.Get(),
        0,
        0,
        0,
        0,
        source,
        D3D11CalcSubresource(
            0,
            static_cast<UINT>(array_index),
            source_desc.MipLevels),
        nullptr);
    if (!snapshot_pool->wait_for_copy_completion(device.Get(), context.Get())) {
        snapshot_pool->discard();
        return std::nullopt;
    }

    auto snapshot_ref = std::make_shared<D3D11SnapshotFrameRef>();
    snapshot_ref->pool = snapshot_pool;
    snapshot_ref->texture = snapshot;

    TextureFrame result = metadata;
    result.texture_handle = snapshot.Get();
    result.is_ref = true;
    result.is_nv12 = true;
    result.is_p010 = p010_compatible_surface(source_desc.Format);
    result.texture_array_index = 0;
    result.hw_frame_ref = snapshot_ref;
    result.storage = WindowsD3D11FrameStorage{
        snapshot.Get(),
        0,
        result.is_p010,
        static_cast<int>(source_desc.Width),
        static_cast<int>(source_desc.Height),
        snapshot_ref,
    };
    return result;
}

D3D11SnapshotPoolStats d3d11_snapshot_pool_stats(
    const std::shared_ptr<D3D11SnapshotPool>& snapshot_pool) {
    return snapshot_pool ? snapshot_pool->stats() : D3D11SnapshotPoolStats{};
}

} // namespace vr
