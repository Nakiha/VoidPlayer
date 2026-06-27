#include <catch2/catch_test_macros.hpp>

#include "renderer/layout/layout_validation.h"
#include "renderer/layout/layout_controller.h"
#include "renderer/layout/layout_geometry.h"
#include "renderer/render/render_loop_controller.h"
#include "renderer/decode/hw/hw_decode_provider.h"
#include "renderer/render/presentation_backend_factory.h"
#include "renderer/renderer_config_validation.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include "windows/d3d11/shared_fp16_ring.h"
#include "windows/d3d11/shared_source_cache_ring.h"
#include "windows/wgpu/wgpu_d3d12_ffi_bridge.h"

#include <d3d11_1.h>
#include <d3d12.h>
#include <wrl/client.h>
#endif

using namespace vr;

namespace {

RendererConfig valid_windowed_config() {
    RendererConfig config;
    config.video_paths = {"C:/video.mp4"};
    config.hwnd = reinterpret_cast<void*>(0x1234);
    config.width = 1920;
    config.height = 1080;
    config.use_hardware_decode = true;
    return config;
}

bool layout_float_near(float lhs, float rhs, float epsilon = 0.0001f) {
    return std::fabs(lhs - rhs) <= epsilon;
}

#ifdef _WIN32
struct WgpuD3D12RendererHandle {
    VPWgpuD3D12Renderer* renderer = nullptr;
    ~WgpuD3D12RendererHandle() {
        if (renderer) {
            VPWgpuD3D12RendererDestroy(renderer);
        }
    }
};

Microsoft::WRL::ComPtr<ID3D12Resource> create_probe_rgba16_target(
    ID3D12Device* device,
    UINT width,
    UINT height) {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    if (!device) {
        return texture;
    }
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clear.Color[0] = 0.0f;
    clear.Color[1] = 0.0f;
    clear.Color[2] = 0.0f;
    clear.Color[3] = 1.0f;

    const HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        &clear,
        IID_PPV_ARGS(&texture));
    return SUCCEEDED(hr) ? texture : nullptr;
}

Microsoft::WRL::ComPtr<ID3D12Resource> create_probe_bgra8_target(
    ID3D12Device* device,
    UINT width,
    UINT height) {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    if (!device) {
        return texture;
    }
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    clear.Color[3] = 1.0f;

    const HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        &clear,
        IID_PPV_ARGS(&texture));
    return SUCCEEDED(hr) ? texture : nullptr;
}

Microsoft::WRL::ComPtr<ID3D12Resource> create_probe_nv12_source(
    ID3D12Device* device,
    UINT width,
    UINT height,
    UINT array_layers = 1) {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    if (!device) {
        return texture;
    }
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = static_cast<UINT16>(array_layers);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&texture));
    return SUCCEEDED(hr) ? texture : nullptr;
}

bool readback_probe_bgra8(
    ID3D12Resource* texture,
    UINT width,
    UINT height,
    std::vector<uint8_t>& bgra,
    std::string& error) {
    bgra.clear();
    if (!texture || width == 0 || height == 0) {
        error = "invalid bgra8 readback source";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HRESULT hr = texture->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        error = "failed to query bgra8 readback device";
        return false;
    }

    const D3D12_RESOURCE_DESC source_desc = texture->GetDesc();
    if (source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        error = "bgra8 readback source format mismatch";
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT num_rows = 0;
    UINT64 row_size_bytes = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&source_desc,
                                  0,
                                  1,
                                  0,
                                  &footprint,
                                  &num_rows,
                                  &row_size_bytes,
                                  &total_bytes);
    if (total_bytes == 0 || num_rows == 0) {
        error = "bgra8 readback footprint is empty";
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
        error = "bgra8 readback allocation failed";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr) || !queue) {
        error = "bgra8 readback queue creation failed";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr) || !allocator) {
        error = "bgra8 readback allocator creation failed";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
    hr = device->CreateCommandList(0,
                                   D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   allocator.Get(),
                                   nullptr,
                                   IID_PPV_ARGS(&command_list));
    if (FAILED(hr) || !command_list) {
        error = "bgra8 readback command list creation failed";
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
        error = "bgra8 readback command list close failed";
        return false;
    }
    ID3D12CommandList* lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr) || !fence) {
        error = "bgra8 readback fence creation failed";
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        error = "bgra8 readback fence event creation failed";
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
        error = "bgra8 readback GPU wait failed";
        return false;
    }

    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
    hr = readback->Map(0, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
        error = "bgra8 readback map failed";
        return false;
    }

    bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const auto* src_bytes = static_cast<const uint8_t*>(mapped) + footprint.Offset;
    for (UINT row_index = 0; row_index < height; ++row_index) {
        const auto* row =
            src_bytes + static_cast<size_t>(row_index) * footprint.Footprint.RowPitch;
        std::memcpy(bgra.data() + static_cast<size_t>(row_index) * width * 4,
                    row,
                    static_cast<size_t>(width) * 4);
    }
    D3D12_RANGE written_range{0, 0};
    readback->Unmap(0, &written_range);
    return true;
}
#endif

} // namespace

TEST_CASE("Renderer config validation accepts valid windowed config",
          "[renderer_config]") {
    const auto result = validate_renderer_config(valid_windowed_config());
    REQUIRE(result.ok);
}

TEST_CASE("Renderer config validation rejects invalid dimensions",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.width = 0;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.height = kMaxRendererDimension + 1;
    REQUIRE_FALSE(validate_renderer_config(config).ok);
}

TEST_CASE("Renderer config validation rejects invalid path lists",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.video_paths.clear();
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.video_paths = {""};
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.video_paths.assign(kMaxRendererVideoPaths + 1, "C:/video.mp4");
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config = valid_windowed_config();
    config.video_paths = {std::string("\xC3\x28", 2)};
    REQUIRE_FALSE(validate_renderer_config(config).ok);
}

TEST_CASE("Renderer config validation enforces headless interop shape",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.headless = true;
    config.backend.type = RendererBackendType::D3D11;
    config.backend.adapter = nullptr;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config.hwnd = nullptr;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config.backend.adapter = reinterpret_cast<void*>(0x5678);
    REQUIRE(validate_renderer_config(config).ok);
}

TEST_CASE("Renderer config validation accepts WgpuMetal headless output interop",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.headless = true;
    config.hwnd = nullptr;
    config.backend.type = RendererBackendType::WgpuMetal;
    config.backend.output = reinterpret_cast<void*>(0x9abc);

#ifdef __APPLE__
    REQUIRE(validate_renderer_config(config).ok);

    config.backend.output = nullptr;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config.backend.output = reinterpret_cast<void*>(0x9abc);
    config.backend.max_track_slots = kMaxRendererVideoPaths + 1;
    REQUIRE_FALSE(validate_renderer_config(config).ok);
#else
    REQUIRE_FALSE(validate_renderer_config(config).ok);
#endif
}

TEST_CASE("Renderer config validation accepts WgpuD3D12 headless output interop",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.headless = true;
    config.hwnd = nullptr;
    config.backend.type = RendererBackendType::WgpuD3D12;
    config.backend.output = reinterpret_cast<void*>(0x9abc);

#ifdef _WIN32
    REQUIRE(validate_renderer_config(config).ok);

    config.backend.output = nullptr;
    REQUIRE_FALSE(validate_renderer_config(config).ok);

    config.backend.output = reinterpret_cast<void*>(0x9abc);
    config.backend.max_track_slots = kMaxRendererVideoPaths + 1;
    REQUIRE_FALSE(validate_renderer_config(config).ok);
#else
    REQUIRE_FALSE(validate_renderer_config(config).ok);
#endif
}

TEST_CASE("Renderer config validation rejects removed Metal headless backend",
          "[renderer_config]") {
    auto config = valid_windowed_config();
    config.headless = true;
    config.hwnd = nullptr;
    config.backend.type = RendererBackendType::Metal;
    config.backend.output = reinterpret_cast<void*>(0x9abc);

    REQUIRE_FALSE(validate_renderer_config(config).ok);
}

TEST_CASE("Hardware decode provider compatibility keeps Windows on D3D12VA",
          "[renderer_config][hw_decode]") {
    auto d3d11_names = compatible_hw_decode_provider_names(
        RenderBackendKind::D3D11,
        DecodeDeviceMode::IndependentDevice);
    auto d3d12_names = compatible_hw_decode_provider_names(
        RenderBackendKind::WgpuD3D12,
        DecodeDeviceMode::IndependentDevice);

#ifdef _WIN32
    REQUIRE(d3d11_names.empty());

    bool has_d3d12va = false;
    for (const auto* name : d3d12_names) {
        has_d3d12va = has_d3d12va || std::string(name) == "D3D12VA";
        REQUIRE(std::string(name) != "D3D11VA");
    }
    REQUIRE(has_d3d12va);
    REQUIRE(std::string(hw_decode_type_name(HwDecodeType::D3D12VA)) == "D3D12VA");
#else
    REQUIRE(d3d12_names.empty());
#endif
}

TEST_CASE("Windows wgpu-d3d12 presentation backend initializes without D3D11 fallback",
          "[renderer_config][presentation_backend]") {
#ifdef _WIN32
    auto backend = create_presentation_backend(RenderBackendKind::WgpuD3D12);
    REQUIRE(backend != nullptr);
    REQUIRE(backend->kind() == PresentationBackendKind::WgpuD3D12);

    PresentationBackendConfig config;
    config.headless = true;
    config.output = reinterpret_cast<void*>(0x1234);
    config.width = 64;
    config.height = 64;
    const bool initialized = backend->initialize(config);
    REQUIRE(backend->kind() == PresentationBackendKind::WgpuD3D12);
    const auto diagnostics = backend->diagnostics();
    REQUIRE(diagnostics.backend == "wgpu-d3d12");
    if (initialized) {
        REQUIRE(backend->native_render_device() != nullptr);
        REQUIRE(backend->native_render_command_queue() != nullptr);
        REQUIRE(diagnostics.fallback_reason == "none");
        REQUIRE_FALSE(backend->draw_frame(RendererDrawSnapshot{},
                                          PresentationBackendDrawHooks{}));
        REQUIRE(std::string(backend->last_error()).find("shared FP16 output") !=
                std::string::npos);
    } else {
        REQUIRE(backend->native_render_device() == nullptr);
        REQUIRE(backend->native_render_command_queue() == nullptr);
        REQUIRE_FALSE(diagnostics.fallback_reason.empty());
    }
#else
    REQUIRE(create_presentation_backend(RenderBackendKind::WgpuD3D12) == nullptr);
#endif
}

TEST_CASE("Windows wgpu-d3d12 imports and clears a D3D12 render target",
          "[renderer_config][presentation_backend][wgpu_d3d12]") {
#ifdef _WIN32
    std::array<char, 512> error{};
    WgpuD3D12RendererHandle handle{
        VPWgpuD3D12RendererCreate(error.data(), error.size())};
    if (!handle.renderer) {
        SKIP(std::string("wgpu-d3d12 renderer unavailable: ") + error.data());
    }
    auto* device = static_cast<ID3D12Device*>(
        VPWgpuD3D12RendererD3D12Device(handle.renderer));
    auto* queue = static_cast<ID3D12CommandQueue*>(
        VPWgpuD3D12RendererD3D12CommandQueue(handle.renderer));
    REQUIRE(device != nullptr);
    REQUIRE(queue != nullptr);

    constexpr UINT kWidth = 16;
    constexpr UINT kHeight = 16;
    auto target = create_probe_rgba16_target(device, kWidth, kHeight);
    REQUIRE(target != nullptr);

    error.fill(0);
    VPWgpuD3D12RenderTargetClearRequest request = {};
    request.d3d12_resource = target.Get();
    request.format = VP_WGPU_D3D12_TEXTURE_FORMAT_RGBA16_FLOAT;
    request.width = kWidth;
    request.height = kHeight;
    request.color[0] = 0.25f;
    request.color[1] = 0.5f;
    request.color[2] = 0.75f;
    request.color[3] = 1.0f;
    request.error = error.data();
    request.error_size = error.size();
    INFO(error.data());
    REQUIRE(VPWgpuD3D12RendererClearRenderTargetForProbe(
                handle.renderer, &request) == 0);

    VPWgpuD3D12ProfilerSnapshot profiler = {};
    REQUIRE(VPWgpuD3D12RendererGetProfilerSnapshot(
                handle.renderer, &profiler) == 0);
    REQUIRE(profiler.destination_import_count >= 1);
    REQUIRE(profiler.submit_count >= 1);
#else
    SUCCEED("wgpu-d3d12 render target import is Windows-only");
#endif
}

TEST_CASE("Windows wgpu-d3d12 composites Flutter BGRA overlay input",
          "[renderer_config][presentation_backend][wgpu_d3d12]") {
#ifdef _WIN32
    std::array<char, 512> error{};
    WgpuD3D12RendererHandle handle{
        VPWgpuD3D12RendererCreate(error.data(), error.size())};
    if (!handle.renderer) {
        SKIP(std::string("wgpu-d3d12 renderer unavailable: ") + error.data());
    }
    auto* device = static_cast<ID3D12Device*>(
        VPWgpuD3D12RendererD3D12Device(handle.renderer));
    REQUIRE(device != nullptr);

    constexpr UINT kWidth = 16;
    constexpr UINT kHeight = 16;
    auto destination = create_probe_bgra8_target(device, kWidth, kHeight);
    auto flutter = create_probe_bgra8_target(device, kWidth, kHeight);
    REQUIRE(destination != nullptr);
    REQUIRE(flutter != nullptr);

    VPWgpuD3D12RenderTargetClearRequest clear = {};
    clear.d3d12_resource = flutter.Get();
    clear.format = VP_WGPU_D3D12_TEXTURE_FORMAT_BGRA8_UNORM;
    clear.width = kWidth;
    clear.height = kHeight;
    clear.color[0] = 0.5f;
    clear.color[1] = 0.0f;
    clear.color[2] = 0.0f;
    clear.color[3] = 0.5f;
    clear.error = error.data();
    clear.error_size = error.size();
    INFO(error.data());
    REQUIRE(VPWgpuD3D12RendererClearRenderTargetForProbe(
                handle.renderer, &clear) == 0);

    std::vector<uint8_t> y_plane(kWidth * kHeight, 16);
    std::vector<uint8_t> uv_plane((kWidth / 2) * (kHeight / 2) * 2, 128);

    VPWgpuD3D12PresentDecisionInfo decision = {};
    decision.should_present = 1;
    decision.frame_count = 1;
    decision.track_count = 1;
    decision.mode = LAYOUT_SIDE_BY_SIDE;
    decision.split_pos = 0.5f;
    decision.background_color[3] = 1.0f;
    for (int i = 0; i < 4; ++i) {
        decision.order[i] = i;
        decision.inv_display_size_x[i] = 1.0f;
        decision.inv_display_size_y[i] = 1.0f;
        decision.source_width[i] = 1;
        decision.source_height[i] = 1;
        decision.coded_width[i] = 1;
        decision.coded_height[i] = 1;
        decision.nv12_uv_scale_x[i] = 1.0f;
        decision.nv12_uv_scale_y[i] = 1.0f;
        decision.color_range[i] = VIDEO_COLOR_RANGE_LIMITED;
        decision.color_matrix[i] = VIDEO_COLOR_MATRIX_BT709;
        decision.color_transfer[i] = VIDEO_COLOR_TRANSFER_SDR;
        decision.color_primaries[i] = VIDEO_COLOR_PRIMARIES_BT709;
    }
    decision.frames[0].present = 1;
    decision.frames[0].slot = 0;
    decision.frames[0].width = static_cast<int32_t>(kWidth);
    decision.frames[0].height = static_cast<int32_t>(kHeight);
    decision.source_width[0] = static_cast<int32_t>(kWidth);
    decision.source_height[0] = static_cast<int32_t>(kHeight);
    decision.coded_width[0] = static_cast<int32_t>(kWidth);
    decision.coded_height[0] = static_cast<int32_t>(kHeight);
    decision.yuv_format[0] = VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
    decision.y_stride[0] = static_cast<int32_t>(kWidth);
    decision.uv_stride[0] = static_cast<int32_t>(kWidth);

    VPWgpuD3D12CompositeRequest request = {};
    request.destination_resource = destination.Get();
    request.output_format = VP_WGPU_D3D12_TEXTURE_FORMAT_BGRA8_UNORM;
    request.output_color_mode = VP_WGPU_D3D12_OUTPUT_COLOR_MODE_SDR;
    request.flutter_resource = flutter.Get();
    request.flutter_format = VP_WGPU_D3D12_TEXTURE_FORMAT_BGRA8_UNORM;
    request.flutter_width = kWidth;
    request.flutter_height = kHeight;
    request.source_formats[0] = VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
    request.cpu_sources[0].y_data = y_plane.data();
    request.cpu_sources[0].y_size = y_plane.size();
    request.cpu_sources[0].uv_data = uv_plane.data();
    request.cpu_sources[0].uv_size = uv_plane.size();
    request.cpu_sources[0].format = VP_WGPU_D3D12_TEXTURE_FORMAT_NV12;
    request.cpu_sources[0].y_stride = static_cast<int32_t>(kWidth);
    request.cpu_sources[0].uv_stride = static_cast<int32_t>(kWidth);
    request.cpu_sources[0].y_width = kWidth;
    request.cpu_sources[0].y_height = kHeight;
    request.cpu_sources[0].uv_width = kWidth / 2;
    request.cpu_sources[0].uv_height = kHeight / 2;
    request.decision = &decision;
    request.width = static_cast<int32_t>(kWidth);
    request.height = static_cast<int32_t>(kHeight);
    request.error = error.data();
    request.error_size = error.size();

    VPWgpuD3D12ProfilerSnapshot before = {};
    REQUIRE(VPWgpuD3D12RendererGetProfilerSnapshot(
                handle.renderer, &before) == 0);
    error.fill(0);
    INFO(error.data());
    REQUIRE(VPWgpuD3D12RendererRenderComposite(
                handle.renderer, &request) == 0);
    VPWgpuD3D12ProfilerSnapshot after = {};
    REQUIRE(VPWgpuD3D12RendererGetProfilerSnapshot(
                handle.renderer, &after) == 0);
    REQUIRE(after.destination_import_count > before.destination_import_count);
    REQUIRE(after.source_import_count >= before.source_import_count + 2);
    REQUIRE(after.submit_count > before.submit_count);

    std::vector<uint8_t> pixels;
    std::string readback_error;
    REQUIRE(readback_probe_bgra8(destination.Get(),
                                 kWidth,
                                 kHeight,
                                 pixels,
                                 readback_error));
    const size_t center =
        ((static_cast<size_t>(kHeight / 2) * kWidth) + (kWidth / 2)) * 4;
    INFO("readback_error=" << readback_error);
    INFO("center bgra=(" << static_cast<int>(pixels[center + 0]) << ", "
                         << static_cast<int>(pixels[center + 1]) << ", "
                         << static_cast<int>(pixels[center + 2]) << ", "
                         << static_cast<int>(pixels[center + 3]) << ")");
    REQUIRE(pixels[center + 2] > 80);
    REQUIRE(pixels[center + 2] > pixels[center + 1] + 40);
    REQUIRE(pixels[center + 2] > pixels[center + 0] + 40);
    REQUIRE(pixels[center + 3] > 200);
#else
    SUCCEED("wgpu-d3d12 Flutter overlay composite is Windows-only");
#endif
}

TEST_CASE("Windows wgpu-d3d12 backend publishes shared FP16 output",
          "[renderer_config][presentation_backend][wgpu_d3d12]") {
#ifdef _WIN32
    auto backend = create_presentation_backend(RenderBackendKind::WgpuD3D12);
    REQUIRE(backend != nullptr);

    PresentationBackendConfig config;
    config.headless = true;
    config.shared_fp16_output = true;
    config.width = 32;
    config.height = 24;
    if (!backend->initialize(config)) {
        SKIP(std::string("wgpu-d3d12 backend unavailable: ") +
             backend->last_error());
    }

    RendererDrawSnapshot snapshot;
    snapshot.target_width = 32;
    snapshot.target_height = 24;
    snapshot.background_color[0] = 0.1f;
    snapshot.background_color[1] = 0.2f;
    snapshot.background_color[2] = 0.3f;
    snapshot.background_color[3] = 1.0f;
    REQUIRE(backend->draw_frame(snapshot, PresentationBackendDrawHooks{}));

    SharedFp16TextureSnapshot shared;
    REQUIRE(backend->acquire_shared_fp16_texture(shared));
    REQUIRE(shared.handle != nullptr);
    REQUIRE(shared.width == 32);
    REQUIRE(shared.height == 24);
    REQUIRE(shared.buffer_index >= 0);
    REQUIRE(shared.ring_generation != 0);
    REQUIRE(shared.frame_generation != 0);
    REQUIRE(shared.sync_mode ==
            SharedFp16TextureSyncMode::PublishedAfterProducerWait);

    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context;
    D3D_FEATURE_LEVEL level = {};
    REQUIRE(SUCCEEDED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &d3d11_device, &level, &d3d11_context)));
    Microsoft::WRL::ComPtr<ID3D11Device1> d3d11_device1;
    REQUIRE(SUCCEEDED(d3d11_device.As(&d3d11_device1)));
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11_texture;
    REQUIRE(SUCCEEDED(d3d11_device1->OpenSharedResource1(
        shared.handle, IID_PPV_ARGS(&d3d11_texture))));
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    REQUIRE(SUCCEEDED(d3d11_device->CreateShaderResourceView(
        d3d11_texture.Get(), nullptr, &srv)));

    backend->release_shared_fp16_texture(
        shared.buffer_index, shared.ring_generation);
#else
    SUCCEED("wgpu-d3d12 shared FP16 output is Windows-only");
#endif
}

TEST_CASE("Windows wgpu-d3d12 backend composites a D3D12VA NV12 source",
          "[renderer_config][presentation_backend][wgpu_d3d12]") {
#ifdef _WIN32
    auto backend = create_presentation_backend(RenderBackendKind::WgpuD3D12);
    REQUIRE(backend != nullptr);

    PresentationBackendConfig config;
    config.headless = true;
    config.shared_fp16_output = true;
    config.width = 64;
    config.height = 48;
    if (!backend->initialize(config)) {
        SKIP(std::string("wgpu-d3d12 backend unavailable: ") +
             backend->last_error());
    }
    auto* device = static_cast<ID3D12Device*>(backend->native_render_device());
    REQUIRE(device != nullptr);

    constexpr int kSourceWidth = 32;
    constexpr int kSourceHeight = 24;
    auto source = create_probe_nv12_source(device, kSourceWidth, kSourceHeight);
    REQUIRE(source != nullptr);

    TextureFrame frame;
    frame.width = kSourceWidth;
    frame.height = kSourceHeight;
    frame.pts_us = 0;
    frame.duration_us = 16667;
    frame.is_nv12 = true;
    frame.color.range = VIDEO_COLOR_RANGE_LIMITED;
    frame.color.matrix = VIDEO_COLOR_MATRIX_BT709;
    frame.color.transfer = VIDEO_COLOR_TRANSFER_SDR;
    frame.color.primaries = VIDEO_COLOR_PRIMARIES_BT709;
    frame.storage = D3D12TextureFrameStorage{
        source.Get(),
        0,
        nullptr,
        nullptr,
        0,
        false,
        false,
        kSourceWidth,
        kSourceHeight,
        {},
    };

    RendererDrawSnapshot snapshot;
    snapshot.target_width = 64;
    snapshot.target_height = 48;
    snapshot.background_color[3] = 1.0f;
    snapshot.layout.mode = LAYOUT_SIDE_BY_SIDE;
    snapshot.layout.order[0] = 0;
    snapshot.layout.order[1] = 1;
    snapshot.layout.order[2] = 2;
    snapshot.layout.order[3] = 3;
    snapshot.track_geometry[0] = {
        true,
        kSourceWidth,
        kSourceHeight,
        static_cast<float>(kSourceWidth) / static_cast<float>(kSourceHeight),
    };
    snapshot.decision.should_present = true;
    snapshot.decision.file_ids[0] = 0;
    snapshot.decision.frames[0] = frame;
    REQUIRE(backend->draw_frame(snapshot, PresentationBackendDrawHooks{}));

    SharedFp16TextureSnapshot shared;
    REQUIRE(backend->acquire_shared_fp16_texture(shared));
    REQUIRE(shared.handle != nullptr);
    REQUIRE(shared.width == 64);
    REQUIRE(shared.height == 48);
    REQUIRE(shared.sync_mode ==
            SharedFp16TextureSyncMode::PublishedAfterProducerWait);
    backend->release_shared_fp16_texture(
        shared.buffer_index, shared.ring_generation);
#else
    SUCCEED("wgpu-d3d12 D3D12VA composite is Windows-only");
#endif
}

TEST_CASE("Windows wgpu-d3d12 backend composites a CPU planar YUV420 source",
          "[renderer_config][presentation_backend][wgpu_d3d12]") {
#ifdef _WIN32
    auto backend = create_presentation_backend(RenderBackendKind::WgpuD3D12);
    REQUIRE(backend != nullptr);

    PresentationBackendConfig config;
    config.headless = true;
    config.shared_fp16_output = true;
    config.width = 64;
    config.height = 48;
    if (!backend->initialize(config)) {
        SKIP(std::string("wgpu-d3d12 backend unavailable: ") +
             backend->last_error());
    }

    constexpr int kSourceWidth = 32;
    constexpr int kSourceHeight = 24;
    constexpr int kChromaWidth = kSourceWidth / 2;
    constexpr int kChromaHeight = kSourceHeight / 2;
    auto y_plane = std::make_shared<std::vector<uint8_t>>(
        kSourceWidth * kSourceHeight, 180);
    auto u_plane = std::make_shared<std::vector<uint8_t>>(
        kChromaWidth * kChromaHeight, 96);
    auto v_plane = std::make_shared<std::vector<uint8_t>>(
        kChromaWidth * kChromaHeight, 160);

    TextureFrame frame;
    frame.width = kSourceWidth;
    frame.height = kSourceHeight;
    frame.pts_us = 0;
    frame.duration_us = 16667;
    frame.is_nv12 = false;
    frame.color.range = VIDEO_COLOR_RANGE_LIMITED;
    frame.color.matrix = VIDEO_COLOR_MATRIX_BT709;
    frame.color.transfer = VIDEO_COLOR_TRANSFER_SDR;
    frame.color.primaries = VIDEO_COLOR_PRIMARIES_BT709;
    frame.storage = CpuPlanarYuvFrameStorage{
        {},
        {y_plane->data(), u_plane->data(), v_plane->data()},
        {kSourceWidth, kChromaWidth, kChromaWidth},
        {kSourceWidth, kChromaWidth, kChromaWidth},
        {kSourceHeight, kChromaHeight, kChromaHeight},
        1,
    };

    RendererDrawSnapshot snapshot;
    snapshot.target_width = 64;
    snapshot.target_height = 48;
    snapshot.background_color[3] = 1.0f;
    snapshot.layout.mode = LAYOUT_SIDE_BY_SIDE;
    snapshot.layout.order[0] = 0;
    snapshot.layout.order[1] = 1;
    snapshot.layout.order[2] = 2;
    snapshot.layout.order[3] = 3;
    snapshot.track_geometry[0] = {
        true,
        kSourceWidth,
        kSourceHeight,
        static_cast<float>(kSourceWidth) / static_cast<float>(kSourceHeight),
    };
    snapshot.decision.should_present = true;
    snapshot.decision.file_ids[0] = 0;
    snapshot.decision.frames[0] = frame;
    REQUIRE(backend->draw_frame(snapshot, PresentationBackendDrawHooks{}));

    SharedFp16TextureSnapshot shared;
    REQUIRE(backend->acquire_shared_fp16_texture(shared));
    REQUIRE(shared.handle != nullptr);
    REQUIRE(shared.width == 64);
    REQUIRE(shared.height == 48);
    REQUIRE(shared.sync_mode ==
            SharedFp16TextureSyncMode::PublishedAfterProducerWait);
    backend->release_shared_fp16_texture(
        shared.buffer_index, shared.ring_generation);
#else
    SUCCEED("wgpu-d3d12 CPU planar composite is Windows-only");
#endif
}

TEST_CASE("Windows wgpu-d3d12 backend publishes retained source-cache bundles",
          "[renderer_config][presentation_backend][wgpu_d3d12][windows_source_cache]") {
#ifdef _WIN32
    auto backend = create_presentation_backend(RenderBackendKind::WgpuD3D12);
    REQUIRE(backend != nullptr);

    PresentationBackendConfig config;
    config.headless = true;
    config.shared_fp16_output = true;
    config.width = 64;
    config.height = 48;
    if (!backend->initialize(config)) {
        SKIP(std::string("wgpu-d3d12 backend unavailable: ") +
             backend->last_error());
    }

    constexpr int kFileId = 42;
    constexpr int kSourceWidth = 32;
    constexpr int kSourceHeight = 24;
    constexpr int kChromaWidth = kSourceWidth / 2;
    constexpr int kChromaHeight = kSourceHeight / 2;
    auto y_plane = std::make_shared<std::vector<uint8_t>>(
        kSourceWidth * kSourceHeight, 180);
    auto u_plane = std::make_shared<std::vector<uint8_t>>(
        kChromaWidth * kChromaHeight, 96);
    auto v_plane = std::make_shared<std::vector<uint8_t>>(
        kChromaWidth * kChromaHeight, 160);

    REQUIRE(backend->configure_source_cache({
        SourceCacheTrackDescriptor{
            0,
            kFileId,
            kSourceWidth,
            kSourceHeight,
            VIDEO_COLOR_TRANSFER_SDR,
        },
    }));

    TextureFrame frame;
    frame.width = kSourceWidth;
    frame.height = kSourceHeight;
    frame.pts_us = 0;
    frame.duration_us = 16667;
    frame.is_nv12 = false;
    frame.color.range = VIDEO_COLOR_RANGE_LIMITED;
    frame.color.matrix = VIDEO_COLOR_MATRIX_BT709;
    frame.color.transfer = VIDEO_COLOR_TRANSFER_SDR;
    frame.color.primaries = VIDEO_COLOR_PRIMARIES_BT709;
    frame.storage = CpuPlanarYuvFrameStorage{
        {},
        {y_plane->data(), u_plane->data(), v_plane->data()},
        {kSourceWidth, kChromaWidth, kChromaWidth},
        {kSourceWidth, kChromaWidth, kChromaWidth},
        {kSourceHeight, kChromaHeight, kChromaHeight},
        1,
    };

    RendererDrawSnapshot snapshot;
    snapshot.target_width = 64;
    snapshot.target_height = 48;
    snapshot.background_color[3] = 1.0f;
    snapshot.layout.mode = LAYOUT_SIDE_BY_SIDE;
    snapshot.layout.order[0] = kFileId;
    snapshot.layout.order[1] = -1;
    snapshot.layout.order[2] = -1;
    snapshot.layout.order[3] = -1;
    snapshot.tracks[0].active = true;
    snapshot.tracks[0].file_id = kFileId;
    snapshot.tracks[0].video_width = kSourceWidth;
    snapshot.tracks[0].video_height = kSourceHeight;
    snapshot.tracks[0].video_aspect =
        static_cast<float>(kSourceWidth) / static_cast<float>(kSourceHeight);
    snapshot.track_geometry[0] = {
        true,
        kSourceWidth,
        kSourceHeight,
        snapshot.tracks[0].video_aspect,
    };
    snapshot.decision.should_present = true;
    snapshot.decision.file_ids[0] = kFileId;
    snapshot.decision.frames[0] = frame;
    REQUIRE(backend->draw_frame(snapshot, PresentationBackendDrawHooks{}));

    SharedSourceCacheBundleSnapshot bundle;
    REQUIRE(backend->acquire_source_cache_bundle(bundle));
    REQUIRE(bundle.buffer_index >= 0);
    REQUIRE(bundle.ring_generation != 0);
    REQUIRE(bundle.frame_generation != 0);
    REQUIRE(bundle.texture_count == 1);
    REQUIRE(bundle.textures[0].handle != nullptr);
    REQUIRE(bundle.textures[0].source_slot == 0);
    REQUIRE(bundle.textures[0].source_file_id == kFileId);
    REQUIRE(bundle.textures[0].width == kSourceWidth);
    REQUIRE(bundle.textures[0].height == kSourceHeight);
    REQUIRE(bundle.textures[0].sync_mode ==
            SharedSourceCacheTextureSyncMode::PublishedAfterProducerWait);
    REQUIRE(bundle.textures[0].consumer_acquire_key == 0);
    REQUIRE(bundle.textures[0].producer_release_key == 0);

    backend->release_source_cache_bundle(
        bundle.buffer_index, bundle.ring_generation);
#else
    SUCCEED("wgpu-d3d12 source-cache bundles are Windows-only");
#endif
}

TEST_CASE("Renderer config validation covers speed and loop range",
          "[renderer_config]") {
    REQUIRE(validate_playback_speed(1.0).ok);
    REQUIRE(validate_playback_speed(kMaxPlaybackSpeed).ok);
    REQUIRE_FALSE(validate_playback_speed(0.0).ok);
    REQUIRE_FALSE(validate_playback_speed(kMaxPlaybackSpeed + 0.1).ok);

    REQUIRE(validate_loop_range(false, 0, 0).ok);
    REQUIRE(validate_loop_range(true, 0, 1000).ok);
    REQUIRE(validate_loop_range(
        true,
        media_time_from_us(0),
        media_time_from_us(1000)).ok);
    REQUIRE_FALSE(validate_loop_range(true, -1, 1000).ok);
    REQUIRE_FALSE(validate_loop_range(true, 1000, 1000).ok);
}

TEST_CASE("Layout validation enforces viewport guardrails",
          "[renderer_config][layout]") {
    LayoutState layout;
    layout.order[0] = 1;
    layout.order[1] = 2;
    layout.order[2] = -1;
    layout.order[3] = -1;
    REQUIRE(validate_layout_state(layout).ok);

    auto invalid = layout;
    invalid.split_pos = -0.01f;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);

    invalid = layout;
    invalid.split_pos = 1.01f;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);

    invalid = layout;
    invalid.zoom_ratio = kMinLayoutZoomRatio - 0.01f;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);

    invalid = layout;
    invalid.zoom_ratio = kMaxLayoutZoomRatio + 0.01f;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);
}

TEST_CASE("Layout validation treats order entries as file IDs",
          "[renderer_config][layout]") {
    LayoutState layout;
    layout.order[0] = 42;
    layout.order[1] = 99;
    layout.order[2] = -1;
    layout.order[3] = 0;
    REQUIRE(validate_layout_state(layout).ok);

    auto invalid = layout;
    invalid.order[3] = 42;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);

    invalid = layout;
    invalid.order[2] = -2;
    REQUIRE_FALSE(validate_layout_state(invalid).ok);
}

TEST_CASE("LayoutController owns file-id to slot order translation",
          "[renderer_config][layout]") {
    LayoutController controller;
    LayoutState layout;
    controller.reset(layout);

    controller.append_track(layout, 10, 0);
    controller.append_track(layout, 20, 2);
    REQUIRE(layout.order[0] == 0);
    REQUIRE(layout.order[1] == 2);
    REQUIRE(layout.order[2] == 0);
    REQUIRE(layout.order[3] == 0);

    auto snapshot = controller.snapshot(layout);
    REQUIRE(snapshot.order[0] == 10);
    REQUIRE(snapshot.order[1] == 20);
    REQUIRE(snapshot.order[2] == -1);
    REQUIRE(snapshot.order[3] == -1);

    LayoutState requested;
    requested.order[0] = 20;
    requested.order[1] = 10;
    requested.order[2] = 99;
    requested.order[3] = -1;
    controller.apply(layout, requested, [](int file_id) {
        if (file_id == 10) return 0;
        if (file_id == 20) return 2;
        return -1;
    });
    REQUIRE(layout.order[0] == 2);
    REQUIRE(layout.order[1] == 0);
    REQUIRE(layout.order[2] == 0);
    REQUIRE(layout.order[3] == 0);

    controller.remove_track(layout, 20, [](int file_id) {
        if (file_id == 10) return 0;
        return -1;
    });
    snapshot = controller.snapshot(layout);
    REQUIRE(snapshot.order[0] == 10);
    REQUIRE(snapshot.order[1] == 99);
    REQUIRE(snapshot.order[2] == -1);
    REQUIRE(snapshot.order[3] == -1);
    REQUIRE(layout.order[0] == 0);
    REQUIRE(layout.order[1] == 0);
}

TEST_CASE("Layout geometry computes shader constants outside Renderer",
          "[renderer_config][layout]") {
    LayoutState layout;
    layout.mode = LAYOUT_SIDE_BY_SIDE;
    layout.zoom_ratio = 1.0f;
    layout.pixel_size_mode = PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;
    layout.order[0] = 0;
    layout.order[1] = 1;
    layout.order[2] = 0;
    layout.order[3] = 0;

    LayoutTrackGeometryList tracks = {};
    tracks[0] = {true, 1920, 1080, 16.0f / 9.0f};
    tracks[1] = {true, 1280, 720, 16.0f / 9.0f};

    ShaderConstants constants = {};
    populate_layout_shader_constants(constants, layout, tracks, 2000, 1000);

    REQUIRE(constants.track_count == 2);
    REQUIRE(constants.order[0] == 0);
    REQUIRE(constants.order[1] == 1);
    REQUIRE(layout_float_near(constants.track_scale[0], 1.0f));
    REQUIRE(layout_float_near(constants.track_scale[1], 2.0f / 3.0f));
    REQUIRE(layout_float_near(constants.display_offset_x[0], 0.0f));
    REQUIRE(layout_float_near(constants.display_offset_y[0], 0.21875f));
    REQUIRE(layout_float_near(constants.inv_display_size_x[0], 1.0f));
    REQUIRE(layout_float_near(constants.inv_display_size_y[0], 16.0f / 9.0f));

    const auto display = display_pixel_size_for_layout(2000, 1000, layout, tracks);
    REQUIRE(layout_float_near(display.first, 1000.0f));
    REQUIRE(layout_float_near(display.second, 562.5f));
}

TEST_CASE("Layout geometry owns resize view offset scaling",
          "[renderer_config][layout]") {
    LayoutState layout;
    layout.mode = LAYOUT_SIDE_BY_SIDE;
    layout.zoom_ratio = 1.0f;
    layout.pixel_size_mode = PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;
    layout.view_offset[0] = 100.0f;
    layout.view_offset[1] = -50.0f;
    layout.order[0] = 0;
    layout.order[1] = 1;

    LayoutTrackGeometryList tracks = {};
    tracks[0] = {true, 1920, 1080, 16.0f / 9.0f};
    tracks[1] = {true, 1280, 720, 16.0f / 9.0f};

    const auto adjustment = adjust_layout_view_offset_for_resize(
        layout, 2000, 1000, 4000, 2000, tracks);
    REQUIRE(adjustment.adjusted_x);
    REQUIRE(adjustment.adjusted_y);
    REQUIRE(layout_float_near(adjustment.old_offset_x, 100.0f));
    REQUIRE(layout_float_near(adjustment.old_offset_y, -50.0f));
    REQUIRE(layout_float_near(layout.view_offset[0], 200.0f));
    REQUIRE(layout_float_near(layout.view_offset[1], -100.0f));
    REQUIRE(layout_float_near(adjustment.new_offset_x, 200.0f));
    REQUIRE(layout_float_near(adjustment.new_offset_y, -100.0f));

    layout.view_offset[0] = 12.0f;
    layout.view_offset[1] = 34.0f;
    const auto skipped = adjust_layout_view_offset_for_resize(
        layout, 0, 1000, 4000, 2000, tracks);
    REQUIRE_FALSE(skipped.adjusted_x);
    REQUIRE_FALSE(skipped.adjusted_y);
    REQUIRE(layout_float_near(layout.view_offset[0], 12.0f));
    REQUIRE(layout_float_near(layout.view_offset[1], 34.0f));
}

TEST_CASE("RenderLoopController owns loop timing policy",
          "[renderer_config][render_loop]") {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    RenderLoopController controller;
    const auto t0 = Clock::time_point{};

    REQUIRE(controller.should_apply_resize(t0 + 34ms));
    controller.mark_resize_applied(t0 + 34ms);
    REQUIRE_FALSE(controller.should_apply_resize(t0 + 50ms));
    REQUIRE(controller.should_apply_resize(t0 + 67ms));

    controller.start(t0);
    int64_t pts_delta = -1;
    REQUIRE_FALSE(controller.should_emit_diagnostics(t0 + 1999ms, 5000, pts_delta));
    REQUIRE(controller.should_emit_diagnostics(t0 + 2000ms, 5000, pts_delta));
    REQUIRE(pts_delta == 5000);
    REQUIRE(controller.should_emit_diagnostics(t0 + 4000ms, 9000, pts_delta));
    REQUIRE(pts_delta == 4000);

    REQUIRE(controller.frame_deadline_sleep(1000, 9000, 1.0, 8000).count() == 8000);
    REQUIRE(controller.frame_deadline_sleep(1000, 30000, 1.0, 8000).count() == 8000);
    REQUIRE(controller.frame_deadline_sleep(1000, 5000, 2.0, 8000).count() == 2000);
    REQUIRE(controller.frame_deadline_sleep(5000, 1000, 1.0, 8000).count() == 0);
    REQUIRE(controller.frame_deadline_sleep(1000, 5000, 0.0, 8000).count() == 0);
}

TEST_CASE("Native resource budget exposes renderer guardrails",
          "[renderer_config]") {
    constexpr auto budget = default_native_resource_budget();
    REQUIRE(budget.max_tracks == kMaxRendererVideoPaths);
    REQUIRE(budget.max_dimension == kMaxRendererDimension);
    REQUIRE(budget.max_path_bytes == kMaxRendererPathBytes);
    REQUIRE(budget.max_cpu_frame_bytes == kMaxCpuFrameBytes);
    REQUIRE(budget.max_capture_frame_bytes == kMaxCaptureFrameBytes);
    REQUIRE(budget.max_exact_seek_reorder_frames == kMaxExactSeekReorderFrames);
    REQUIRE(budget.packet_queue_capacity == kDefaultPacketQueueCapacity);
    REQUIRE(budget.high_resolution_track_pixels == kHighResolutionTrackPixels);
    REQUIRE(budget.default_track_forward_depth == kDefaultTrackForwardDepth);
    REQUIRE(budget.default_track_backward_depth == kDefaultTrackBackwardDepth);
    REQUIRE(budget.high_resolution_hardware_track_forward_depth ==
            kHighResolutionHardwareTrackForwardDepth);
    REQUIRE(budget.max_playback_speed == kMaxPlaybackSpeed);
}
