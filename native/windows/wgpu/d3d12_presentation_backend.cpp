#include "windows/wgpu/d3d12_presentation_backend.h"

#include "renderer/render/presentation_snapshot.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mutex>
#include <spdlog/spdlog.h>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace vr {
namespace {

constexpr int kWgpuD3D12MaxTracks = 4;

int d3d12_texture_format_for_storage(const D3D12TextureFrameStorage& storage) {
    return storage.is_p010 ? VP_WGPU_D3D12_TEXTURE_FORMAT_P010
                           : VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
}

size_t required_plane_bytes(size_t stride, size_t row_bytes, int height) {
    if (height <= 0) {
        return 0;
    }
    return stride * static_cast<size_t>(height - 1) + row_bytes;
}

struct WgpuD3D12CpuUploadScratch {
    std::array<std::vector<uint8_t>, kWgpuD3D12MaxTracks> planar_uv;
};

bool fill_cpu_nv12_source(int slot,
                          const TextureFrame& frame,
                          const CpuNv12FrameStorage& storage,
                          VPWgpuD3D12CompositeRequest& composite,
                          VPWgpuD3D12PresentDecisionInfo& decision,
                          std::string& error) {
    const uint8_t* base = storage.data && !storage.data->empty()
        ? storage.data->data()
        : static_cast<const uint8_t*>(frame.texture_handle);
    if (!base) {
        error = "wgpu-d3d12 CPU NV12 source data is null";
        return false;
    }
    const int coded_width = storage.coded_width > 0
        ? storage.coded_width
        : std::max(frame.width + (frame.width & 1), 1);
    const int coded_height = storage.coded_height > 0
        ? storage.coded_height
        : std::max(frame.height + (frame.height & 1), 1);
    const int bytes_per_sample = storage.is_p010 ? 2 : 1;
    const int min_y_stride = coded_width * bytes_per_sample;
    const int min_uv_stride = coded_width * bytes_per_sample;
    const int y_stride = storage.y_stride > 0 ? storage.y_stride : min_y_stride;
    const int uv_stride = storage.uv_stride > 0 ? storage.uv_stride : min_uv_stride;
    if (coded_width <= 0 || coded_height <= 0 || y_stride < min_y_stride ||
        uv_stride < min_uv_stride) {
        error = "wgpu-d3d12 CPU NV12 source geometry is invalid";
        return false;
    }
    const size_t y_bytes = required_plane_bytes(
        static_cast<size_t>(y_stride),
        static_cast<size_t>(min_y_stride),
        coded_height);
    const int uv_height = (coded_height + 1) / 2;
    const size_t uv_bytes = required_plane_bytes(
        static_cast<size_t>(uv_stride),
        static_cast<size_t>(min_uv_stride),
        uv_height);
    const size_t uv_offset = static_cast<size_t>(y_stride) *
                             static_cast<size_t>(coded_height);
    const size_t total_bytes = storage.data ? storage.data->size()
                                            : uv_offset + uv_bytes;
    if (total_bytes < uv_offset + uv_bytes) {
        error = "wgpu-d3d12 CPU NV12 source buffer is too small";
        return false;
    }

    auto& cpu = composite.cpu_sources[slot];
    cpu.y_data = base;
    cpu.y_size = y_bytes;
    cpu.uv_data = base + uv_offset;
    cpu.uv_size = uv_bytes;
    cpu.format = storage.is_p010 ? VP_WGPU_D3D12_TEXTURE_FORMAT_P010
                                 : VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
    cpu.y_stride = y_stride;
    cpu.uv_stride = uv_stride;
    cpu.y_width = static_cast<uint32_t>(coded_width);
    cpu.y_height = static_cast<uint32_t>(coded_height);
    cpu.uv_width = static_cast<uint32_t>((coded_width + 1) / 2);
    cpu.uv_height = static_cast<uint32_t>(uv_height);
    composite.source_formats[slot] = cpu.format;
    decision.yuv_format[slot] = cpu.format;
    decision.coded_width[slot] = coded_width;
    decision.coded_height[slot] = coded_height;
    decision.source_width[slot] = std::max(frame.width, 1);
    decision.source_height[slot] = std::max(frame.height, 1);
    decision.nv12_uv_scale_x[slot] =
        static_cast<float>(decision.source_width[slot]) /
        static_cast<float>(std::max(coded_width, 1));
    decision.nv12_uv_scale_y[slot] =
        static_cast<float>(decision.source_height[slot]) /
        static_cast<float>(std::max(coded_height, 1));
    return true;
}

bool fill_cpu_planar_yuv420_source(int slot,
                                   const TextureFrame& frame,
                                   const CpuPlanarYuvFrameStorage& storage,
                                   VPWgpuD3D12CompositeRequest& composite,
                                   VPWgpuD3D12PresentDecisionInfo& decision,
                                   WgpuD3D12CpuUploadScratch& scratch,
                                   std::string& error) {
    if (storage.bytes_per_sample != 1 || !storage.planes[0] ||
        !storage.planes[1] || !storage.planes[2]) {
        error = "wgpu-d3d12 CPU planar YUV source format is unsupported";
        return false;
    }
    const int y_width = storage.plane_widths[0];
    const int y_height = storage.plane_heights[0];
    const int uv_width = storage.plane_widths[1];
    const int uv_height = storage.plane_heights[1];
    if (y_width <= 0 || y_height <= 0 || uv_width <= 0 || uv_height <= 0 ||
        storage.plane_widths[2] != uv_width ||
        storage.plane_heights[2] != uv_height ||
        storage.strides[0] < y_width ||
        storage.strides[1] < uv_width ||
        storage.strides[2] < uv_width) {
        error = "wgpu-d3d12 CPU planar YUV source geometry is invalid";
        return false;
    }

    const int uv_stride = uv_width * 2;
    auto& packed_uv = scratch.planar_uv[slot];
    packed_uv.resize(static_cast<size_t>(uv_stride) *
                     static_cast<size_t>(uv_height));
    for (int y = 0; y < uv_height; ++y) {
        const uint8_t* src_u =
            storage.planes[1] + static_cast<size_t>(y) * storage.strides[1];
        const uint8_t* src_v =
            storage.planes[2] + static_cast<size_t>(y) * storage.strides[2];
        uint8_t* dst =
            packed_uv.data() + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < uv_width; ++x) {
            dst[x * 2] = src_u[x];
            dst[x * 2 + 1] = src_v[x];
        }
    }

    auto& cpu = composite.cpu_sources[slot];
    cpu.y_data = storage.planes[0];
    cpu.y_size = required_plane_bytes(
        static_cast<size_t>(storage.strides[0]),
        static_cast<size_t>(y_width),
        y_height);
    cpu.uv_data = packed_uv.data();
    cpu.uv_size = packed_uv.size();
    cpu.format = VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
    cpu.y_stride = storage.strides[0];
    cpu.uv_stride = uv_stride;
    cpu.y_width = static_cast<uint32_t>(y_width);
    cpu.y_height = static_cast<uint32_t>(y_height);
    cpu.uv_width = static_cast<uint32_t>(uv_width);
    cpu.uv_height = static_cast<uint32_t>(uv_height);
    composite.source_formats[slot] = cpu.format;
    decision.yuv_format[slot] = cpu.format;
    decision.coded_width[slot] = y_width;
    decision.coded_height[slot] = y_height;
    decision.source_width[slot] = std::max(frame.width, 1);
    decision.source_height[slot] = std::max(frame.height, 1);
    decision.nv12_uv_scale_x[slot] = 1.0f;
    decision.nv12_uv_scale_y[slot] = 1.0f;
    return true;
}

bool wait_for_d3d12_frame_ready(const D3D12TextureFrameStorage& storage,
                                std::string& error) {
    if (!storage.fence || storage.fence_value == 0) {
        return true;
    }
    if (storage.fence->GetCompletedValue() >= storage.fence_value) {
        return true;
    }
    HANDLE event = static_cast<HANDLE>(storage.fence_event);
    HANDLE owned_event = nullptr;
    if (!event) {
        owned_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        event = owned_event;
    }
    if (!event) {
        error = "wgpu-d3d12 source fence event creation failed";
        return false;
    }
    const HRESULT hr =
        storage.fence->SetEventOnCompletion(storage.fence_value, event);
    if (FAILED(hr)) {
        if (owned_event) {
            CloseHandle(owned_event);
        }
        error = "wgpu-d3d12 source fence SetEventOnCompletion failed";
        return false;
    }
    const DWORD wait_result = WaitForSingleObject(event, 1000);
    if (owned_event) {
        CloseHandle(owned_event);
    }
    if (wait_result != WAIT_OBJECT_0) {
        error = "wgpu-d3d12 source fence wait timed out";
        return false;
    }
    return true;
}

float half_to_float(uint16_t value) {
    const uint32_t sign = (value >> 15) & 0x1;
    const uint32_t exp = (value >> 10) & 0x1f;
    const uint32_t mant = value & 0x3ff;
    float result = 0.0f;
    if (exp == 0) {
        result = mant == 0 ? 0.0f
                           : std::ldexp(static_cast<float>(mant) / 1024.0f, -14);
    } else if (exp == 31) {
        result = mant == 0 ? INFINITY : 0.0f;
    } else {
        result = std::ldexp(1.0f + static_cast<float>(mant) / 1024.0f,
                            static_cast<int>(exp) - 15);
    }
    return sign ? -result : result;
}

uint8_t float_to_u8(float value) {
    if (!std::isfinite(value)) {
        value = 0.0f;
    }
    value = std::clamp(value, 0.0f, 1.0f);
    return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

bool copy_rgba16_float_to_bgra8(ID3D12Resource* texture,
                                int width,
                                int height,
                                std::vector<uint8_t>& bgra,
                                std::string& error) {
    bgra.clear();
    if (!texture || width <= 0 || height <= 0) {
        error = "wgpu-d3d12 capture source is invalid";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HRESULT hr = texture->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        error = "wgpu-d3d12 capture failed to query D3D12 device";
        return false;
    }

    const D3D12_RESOURCE_DESC source_desc = texture->GetDesc();
    if (source_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        error = "wgpu-d3d12 capture source format is unsupported";
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT num_rows = 0;
    UINT64 row_size_bytes = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(
        &source_desc, 0, 1, 0, &footprint, &num_rows, &row_size_bytes, &total_bytes);
    if (total_bytes == 0 || num_rows == 0) {
        error = "wgpu-d3d12 capture copy footprint is empty";
        return false;
    }

    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    readback_heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    readback_heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    readback_heap.CreationNodeMask = 1;
    readback_heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    hr = device->CreateCommittedResource(&readback_heap,
                                         D3D12_HEAP_FLAG_NONE,
                                         &buffer_desc,
                                         D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr,
                                         IID_PPV_ARGS(&readback));
    if (FAILED(hr) || !readback) {
        error = "wgpu-d3d12 capture readback allocation failed";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr) || !queue) {
        error = "wgpu-d3d12 capture command queue creation failed";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr) || !allocator) {
        error = "wgpu-d3d12 capture command allocator creation failed";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
    hr = device->CreateCommandList(0,
                                   D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   allocator.Get(),
                                   nullptr,
                                   IID_PPV_ARGS(&command_list));
    if (FAILED(hr) || !command_list) {
        error = "wgpu-d3d12 capture command list creation failed";
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    command_list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    command_list->ResourceBarrier(1, &barrier);

    hr = command_list->Close();
    if (FAILED(hr)) {
        error = "wgpu-d3d12 capture command list close failed";
        return false;
    }
    ID3D12CommandList* lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr) || !fence) {
        error = "wgpu-d3d12 capture fence creation failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        error = "wgpu-d3d12 capture fence event creation failed";
        return false;
    }
    constexpr UINT64 kFenceValue = 1;
    hr = queue->Signal(fence.Get(), kFenceValue);
    if (SUCCEEDED(hr) && fence->GetCompletedValue() < kFenceValue) {
        hr = fence->SetEventOnCompletion(kFenceValue, event);
        if (SUCCEEDED(hr)) {
            const DWORD wait = WaitForSingleObject(event, 1000);
            if (wait != WAIT_OBJECT_0) {
                hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            }
        }
    }
    CloseHandle(event);
    if (FAILED(hr)) {
        error = "wgpu-d3d12 capture GPU readback wait failed";
        return false;
    }

    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
    hr = readback->Map(0, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
        error = "wgpu-d3d12 capture readback map failed";
        return false;
    }

    bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const auto* src_bytes = static_cast<const uint8_t*>(mapped) + footprint.Offset;
    for (int row_index = 0; row_index < height; ++row_index) {
        const auto* row = reinterpret_cast<const uint16_t*>(
            src_bytes + static_cast<size_t>(row_index) * footprint.Footprint.RowPitch);
        for (int x = 0; x < width; ++x) {
            const size_t src_index = static_cast<size_t>(x) * 4;
            const size_t dst_index =
                (static_cast<size_t>(row_index) * static_cast<size_t>(width) +
                 static_cast<size_t>(x)) * 4;
            const float r = half_to_float(row[src_index + 0]);
            const float g = half_to_float(row[src_index + 1]);
            const float b = half_to_float(row[src_index + 2]);
            const float a = half_to_float(row[src_index + 3]);
            bgra[dst_index + 0] = float_to_u8(b);
            bgra[dst_index + 1] = float_to_u8(g);
            bgra[dst_index + 2] = float_to_u8(r);
            bgra[dst_index + 3] = float_to_u8(a);
        }
    }
    D3D12_RANGE written_range{0, 0};
    readback->Unmap(0, &written_range);
    return true;
}

void fill_wgpu_d3d12_decision_from_snapshot(
    const RendererDrawSnapshot& draw_snapshot,
    int width,
    int height,
    VPWgpuD3D12PresentDecisionInfo& out) {
    out = {};
    const auto snapshot = build_presentation_snapshot(
        draw_snapshot.decision,
        draw_snapshot.layout,
        draw_snapshot.track_geometry,
        width,
        height,
        draw_snapshot.background_color);
    const auto& constants = snapshot.constants;
    out.should_present = snapshot.should_present ? 1 : 0;
    out.current_pts_us = snapshot.current_pts_us;
    out.frame_count = snapshot.frame_count;
    out.track_count = constants.track_count;
    out.mode = constants.mode;
    out.split_pos = constants.split_pos;
    for (int i = 0; i < kWgpuD3D12MaxTracks; ++i) {
        out.background_color[i] = constants.background_color[i];
        out.order[i] = constants.order[i];
        out.display_offset_x[i] = constants.display_offset_x[i];
        out.display_offset_y[i] = constants.display_offset_y[i];
        out.inv_display_size_x[i] = constants.inv_display_size_x[i];
        out.inv_display_size_y[i] = constants.inv_display_size_y[i];
        out.view_offset_uv_x[i] = constants.view_offset_uv_x[i];
        out.view_offset_uv_y[i] = constants.view_offset_uv_y[i];

        const auto& frame = snapshot.frames[i];
        auto& frame_out = out.frames[i];
        frame_out.file_id = frame.file_id;
        frame_out.slot = i;
        out.source_width[i] = frame.width > 0 ? frame.width : 1;
        out.source_height[i] = frame.height > 0 ? frame.height : 1;
        out.nv12_uv_scale_x[i] = frame.present ? frame.nv12_uv_scale_x : 1.0f;
        out.nv12_uv_scale_y[i] = frame.present ? frame.nv12_uv_scale_y : 1.0f;
        out.color_range[i] = frame.color_range;
        out.color_matrix[i] = frame.color_matrix;
        out.color_transfer[i] = frame.color_transfer;
        out.color_primaries[i] = frame.color_primaries;
        out.yuv_format[i] = frame.is_p010 ? VP_WGPU_D3D12_TEXTURE_FORMAT_P010
                                           : VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
        out.coded_width[i] = frame.coded_width > 0 ? frame.coded_width
                                                   : out.source_width[i];
        out.coded_height[i] = frame.coded_height > 0 ? frame.coded_height
                                                     : out.source_height[i];
        if (!frame.present) {
            continue;
        }
        frame_out.present = 1;
        frame_out.width = frame.width;
        frame_out.height = frame.height;
        frame_out.pts_us = frame.pts_us;
        frame_out.dts_us = frame.dts_us;
        frame_out.duration_us = frame.duration_us;
        frame_out.analysis_frame_index = frame.analysis_frame_index;
        frame_out.frame_identity_mode = frame.frame_identity_mode;
        frame_out.source_packet_index = frame.source_packet_index;
        frame_out.source_packet_size = frame.source_packet_size;
        frame_out.source_packet_pos = frame.source_packet_pos;
        frame_out.source_packet_pts = frame.source_packet_pts;
        frame_out.source_packet_dts = frame.source_packet_dts;
        frame_out.color_range = frame.color_range;
        frame_out.color_matrix = frame.color_matrix;
        frame_out.color_transfer = frame.color_transfer;
        frame_out.color_primaries = frame.color_primaries;
    }
}

} // namespace

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

    bool latest_texture(Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
                        int& width,
                        int& height) {
        texture.Reset();
        width = 0;
        height = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_ || !latest_generation_ || latest_->writing ||
            !latest_->texture) {
            return false;
        }
        texture = latest_->texture;
        width = latest_generation_->width;
        height = latest_generation_->height;
        return width > 0 && height > 0;
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

bool WgpuD3D12PresentationBackend::capture_front_buffer(
    std::vector<uint8_t>& bgra,
    int& width,
    int& height) {
    bgra.clear();
    width = 0;
    height = 0;
    if (!shared_fp16_ring_) {
        last_error_ = "wgpu-d3d12 capture requires shared FP16 output";
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    if (!shared_fp16_ring_->latest_texture(texture, width, height)) {
        last_error_ = "wgpu-d3d12 capture has no published frame";
        return false;
    }
    std::string error;
    if (!copy_rgba16_float_to_bgra8(texture.Get(), width, height, bgra, error)) {
        last_error_ = error;
        spdlog::error("[WgpuD3D12] {}", last_error_);
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    last_error_.clear();
    return true;
}

bool WgpuD3D12PresentationBackend::capture_front_buffer_region(
    int x,
    int y,
    int width,
    int height,
    std::vector<uint8_t>& bgra,
    int& out_width,
    int& out_height) {
    std::vector<uint8_t> full;
    int full_width = 0;
    int full_height = 0;
    if (!capture_front_buffer(full, full_width, full_height)) {
        bgra.clear();
        out_width = 0;
        out_height = 0;
        return false;
    }
    const int left = std::clamp(x, 0, full_width);
    const int top = std::clamp(y, 0, full_height);
    const int right = std::clamp(x + width, left, full_width);
    const int bottom = std::clamp(y + height, top, full_height);
    out_width = right - left;
    out_height = bottom - top;
    if (out_width <= 0 || out_height <= 0) {
        bgra.clear();
        return false;
    }
    bgra.resize(static_cast<size_t>(out_width) *
                static_cast<size_t>(out_height) * 4);
    for (int row = 0; row < out_height; ++row) {
        const size_t src = (static_cast<size_t>(top + row) *
                            static_cast<size_t>(full_width) +
                            static_cast<size_t>(left)) * 4;
        const size_t dst = static_cast<size_t>(row) *
                           static_cast<size_t>(out_width) * 4;
        std::copy_n(full.data() + src,
                    static_cast<size_t>(out_width) * 4,
                    bgra.data() + dst);
    }
    return true;
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
    VPWgpuD3D12CompositeRequest composite = {};
    VPWgpuD3D12PresentDecisionInfo decision = {};
    WgpuD3D12CpuUploadScratch cpu_scratch;
    fill_wgpu_d3d12_decision_from_snapshot(
        snapshot,
        std::max(snapshot.target_width, 1),
        std::max(snapshot.target_height, 1),
        decision);
    bool has_present_frame = false;
    bool has_composite_source = false;
    for (int slot = 0; slot < kWgpuD3D12MaxTracks; ++slot) {
        if (!snapshot.decision.frames[slot].has_value()) {
            continue;
        }
        has_present_frame = true;
        const auto& frame = *snapshot.decision.frames[slot];
        const auto* storage = frame.d3d12_texture_storage();
        if (storage && storage->texture) {
            std::string wait_error;
            if (!wait_for_d3d12_frame_ready(*storage, wait_error)) {
                shared_fp16_ring_->cancel_frame();
                last_error_ = wait_error;
                spdlog::error("[WgpuD3D12] {}", last_error_);
                return false;
            }
            D3D12_RESOURCE_DESC desc = storage->texture->GetDesc();
            const UINT array_layers = std::max<UINT>(desc.DepthOrArraySize, 1);
            const UINT base_layer = storage->is_texture_array
                ? static_cast<UINT>(std::max(storage->subresource_index, 0))
                : 0;
            composite.source_resources[slot] = storage->texture;
            composite.source_formats[slot] = d3d12_texture_format_for_storage(*storage);
            composite.source_array_layers[slot] = array_layers;
            composite.source_base_array_layers[slot] = std::min(base_layer, array_layers - 1);
            decision.yuv_format[slot] = composite.source_formats[slot];
            decision.coded_width[slot] = storage->coded_width > 0
                ? storage->coded_width
                : std::max(frame.width, 1);
            decision.coded_height[slot] = storage->coded_height > 0
                ? storage->coded_height
                : std::max(frame.height, 1);
            decision.source_width[slot] = std::max(frame.width, 1);
            decision.source_height[slot] = std::max(frame.height, 1);
            decision.nv12_uv_scale_x[slot] =
                static_cast<float>(decision.source_width[slot]) /
                static_cast<float>(std::max(decision.coded_width[slot], 1));
            decision.nv12_uv_scale_y[slot] =
                static_cast<float>(decision.source_height[slot]) /
                static_cast<float>(std::max(decision.coded_height[slot], 1));
            has_composite_source = true;
            continue;
        }
        std::string cpu_error;
        if (const auto* cpu_nv12 = frame.cpu_nv12_storage()) {
            if (!fill_cpu_nv12_source(
                    slot, frame, *cpu_nv12, composite, decision, cpu_error)) {
                shared_fp16_ring_->cancel_frame();
                last_error_ = cpu_error;
                spdlog::error("[WgpuD3D12] {}", last_error_);
                return false;
            }
            has_composite_source = true;
            continue;
        }
        if (const auto* planar_yuv = frame.cpu_planar_yuv_storage()) {
            if (!fill_cpu_planar_yuv420_source(slot,
                                               frame,
                                               *planar_yuv,
                                               composite,
                                               decision,
                                               cpu_scratch,
                                               cpu_error)) {
                shared_fp16_ring_->cancel_frame();
                last_error_ = cpu_error;
                spdlog::error("[WgpuD3D12] {}", last_error_);
                return false;
            }
            has_composite_source = true;
            continue;
        }
        shared_fp16_ring_->cancel_frame();
        last_error_ = "wgpu-d3d12 draw source frame storage is unsupported";
        spdlog::error("[WgpuD3D12] {}", last_error_);
        return false;
    }
    if (has_present_frame && !has_composite_source) {
        shared_fp16_ring_->cancel_frame();
        last_error_ = "wgpu-d3d12 draw requires D3D12VA or CPU YUV source frames";
        return false;
    }
    if (has_composite_source) {
        composite.destination_resource = target;
        composite.output_format = VP_WGPU_D3D12_TEXTURE_FORMAT_RGBA16_FLOAT;
        composite.output_color_mode = VP_WGPU_D3D12_OUTPUT_COLOR_MODE_EDR;
        composite.decision = &decision;
        composite.width = static_cast<int32_t>(std::max(snapshot.target_width, 1));
        composite.height = static_cast<int32_t>(std::max(snapshot.target_height, 1));
        composite.error = error.data();
        composite.error_size = error.size();
        if (VPWgpuD3D12RendererRenderComposite(renderer_, &composite) != 0) {
            shared_fp16_ring_->cancel_frame();
            last_error_ = error.data()[0] != '\0'
                              ? error.data()
                              : "wgpu-d3d12 composite failed";
            spdlog::error("[WgpuD3D12] {}", last_error_);
            return false;
        }
        if (!shared_fp16_ring_->publish_frame()) {
            last_error_ = "wgpu-d3d12 shared FP16 publish failed";
            return false;
        }
        last_error_.clear();
        return true;
    }

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
