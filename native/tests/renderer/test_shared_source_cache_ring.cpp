#include <catch2/catch_test_macros.hpp>

#include "windows/d3d11/shared_source_cache_ring.h"

#include <array>
#include <d3d11.h>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

bool create_device(
    ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context) {
    D3D_FEATURE_LEVEL level = {};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        &level,
        &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device,
            &level,
            &context);
    }
    return SUCCEEDED(hr);
}

std::vector<vr::SourceCacheTrackDescriptor> two_tracks() {
    return {
        {0, 10, 320, 180},
        {1, 11, 160, 90},
    };
}

} // namespace

TEST_CASE("Source cache ring policy selects live frozen and rejected modes",
          "[windows_source_cache][windows_presentation]") {
    const auto descriptors = two_tracks();
    const auto live = vr::resolve_source_cache_ring_policy(
        descriptors, vr::D3D11SharedSourceCacheRing::kDefaultBudgetBytes);
    REQUIRE(live.allowed);
    REQUIRE(live.depth == 3);
    REQUIRE_FALSE(live.frozen_snapshot);

    const auto frozen = vr::resolve_source_cache_ring_policy(
        descriptors, live.bytes_per_frame * 2);
    REQUIRE(frozen.allowed);
    REQUIRE(frozen.depth == 1);
    REQUIRE(frozen.frozen_snapshot);

    const auto rejected = vr::resolve_source_cache_ring_policy(
        descriptors, live.bytes_per_frame - 1);
    REQUIRE_FALSE(rejected.allowed);
}

TEST_CASE("Source cache bundle publication and leases are atomic",
          "[windows_source_cache][windows_presentation]") {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    REQUIRE(create_device(device, context));

    vr::D3D11SharedSourceCacheRing ring;
    REQUIRE(ring.initialize(
        device.Get(), context.Get(), two_tracks()));
    std::array<ID3D11RenderTargetView*, 4> rtvs{};
    size_t count = 0;
    REQUIRE(ring.begin_bundle(rtvs, count));
    REQUIRE(count == 2);
    REQUIRE(rtvs[0] != nullptr);
    REQUIRE(rtvs[1] != nullptr);
    REQUIRE(ring.publish_bundle(nullptr));

    vr::SharedSourceCacheBundleSnapshot first;
    REQUIRE(ring.acquire_latest(first));
    REQUIRE(first.texture_count == 2);
    REQUIRE(first.textures[0].source_file_id == 10);
    REQUIRE(first.textures[1].source_file_id == 11);
    REQUIRE(first.frame_generation == 1);

    REQUIRE(ring.begin_bundle(rtvs, count));
    REQUIRE(ring.publish_bundle(nullptr));
    vr::SharedSourceCacheBundleSnapshot second;
    REQUIRE(ring.acquire_latest(second));
    REQUIRE(second.frame_generation == 2);
    REQUIRE(second.ring_generation == first.ring_generation);

    ring.release(first.buffer_index, first.ring_generation);
    ring.release(second.buffer_index, second.ring_generation);
}

TEST_CASE("Source cache reconfigure retires leased generation",
          "[windows_source_cache][windows_presentation]") {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    REQUIRE(create_device(device, context));

    vr::D3D11SharedSourceCacheRing ring;
    REQUIRE(ring.initialize(
        device.Get(), context.Get(), two_tracks()));
    std::array<ID3D11RenderTargetView*, 4> rtvs{};
    size_t count = 0;
    REQUIRE(ring.begin_bundle(rtvs, count));
    REQUIRE(ring.publish_bundle(nullptr));
    vr::SharedSourceCacheBundleSnapshot old_snapshot;
    REQUIRE(ring.acquire_latest(old_snapshot));

    REQUIRE(ring.reconfigure({{2, 20, 128, 72}}));
    REQUIRE(ring.generation() != old_snapshot.ring_generation);
    vr::SharedSourceCacheBundleSnapshot stale_snapshot;
    REQUIRE_FALSE(ring.acquire_latest(stale_snapshot));
    REQUIRE(ring.begin_bundle(rtvs, count));
    REQUIRE(count == 1);
    REQUIRE(ring.publish_bundle(nullptr));
    ring.release(old_snapshot.buffer_index, old_snapshot.ring_generation);
}

TEST_CASE("Source cache clear retains leased resources until release",
          "[windows_source_cache][windows_presentation]") {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    REQUIRE(create_device(device, context));

    vr::D3D11SharedSourceCacheRing ring;
    REQUIRE(ring.initialize(
        device.Get(), context.Get(), two_tracks()));
    std::array<ID3D11RenderTargetView*, 4> rtvs{};
    size_t count = 0;
    REQUIRE(ring.begin_bundle(rtvs, count));
    REQUIRE(ring.publish_bundle(nullptr));
    vr::SharedSourceCacheBundleSnapshot leased;
    REQUIRE(ring.acquire_latest(leased));

    ring.clear();
    REQUIRE(ring.generation() == 0);
    REQUIRE(ring.texture_count() == 0);
    vr::SharedSourceCacheBundleSnapshot cleared;
    REQUIRE_FALSE(ring.acquire_latest(cleared));
    REQUIRE(ring.estimated_bytes() > 0);

    ring.release(leased.buffer_index, leased.ring_generation);
    REQUIRE(ring.estimated_bytes() == 0);
    REQUIRE(ring.reconfigure({{3, 30, 64, 64}}));
}

TEST_CASE("Frozen source cache publishes only one snapshot",
          "[windows_source_cache][windows_presentation]") {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    REQUIRE(create_device(device, context));

    const auto descriptors = two_tracks();
    const auto policy = vr::resolve_source_cache_ring_policy(
        descriptors, vr::D3D11SharedSourceCacheRing::kDefaultBudgetBytes);
    vr::D3D11SharedSourceCacheRing ring;
    REQUIRE(ring.initialize(
        device.Get(),
        context.Get(),
        descriptors,
        policy.bytes_per_frame * 2));
    REQUIRE(ring.ring_depth() == 1);
    REQUIRE(ring.frozen_snapshot());
    std::array<ID3D11RenderTargetView*, 4> rtvs{};
    size_t count = 0;
    REQUIRE(ring.begin_bundle(rtvs, count));
    REQUIRE(ring.publish_bundle(nullptr));
    REQUIRE_FALSE(ring.begin_bundle(rtvs, count));
}
