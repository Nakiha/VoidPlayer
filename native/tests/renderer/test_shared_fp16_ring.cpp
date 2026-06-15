#include <catch2/catch_test_macros.hpp>

#include "windows/d3d11/shared_fp16_ring.h"

#include <d3d11_1.h>
#include <wrl/client.h>

namespace {

void consume(
    ID3D11Device1* device,
    vr::D3D11SharedFp16Ring& ring,
    const vr::SharedFp16TextureSnapshot& snapshot) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    REQUIRE(SUCCEEDED(device->OpenSharedResource1(
        snapshot.handle, IID_PPV_ARGS(&texture))));
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    REQUIRE(SUCCEEDED(texture.As(&mutex)));
    REQUIRE(mutex->AcquireSync(snapshot.consumer_acquire_key, 100) == S_OK);
    REQUIRE(SUCCEEDED(mutex->ReleaseSync(snapshot.producer_release_key)));
    ring.release(snapshot.buffer_index, snapshot.ring_generation);
}

} // namespace

TEST_CASE("shared FP16 ring preserves leases and resize generations",
          "[windows_dcomp][windows_presentation]") {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = {};
    REQUIRE(SUCCEEDED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, &level, &context)));
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    REQUIRE(SUCCEEDED(device.As(&device1)));

    vr::D3D11SharedFp16Ring ring;
    REQUIRE(ring.initialize(device.Get(), context.Get(), 5, 3));
    REQUIRE(ring.begin_frame() != nullptr);
    REQUIRE(ring.publish_frame());

    vr::SharedFp16TextureSnapshot first;
    REQUIRE(ring.acquire_latest(first));
    REQUIRE(first.width == 5);
    REQUIRE(first.height == 3);
    REQUIRE(first.frame_generation > 0);

    REQUIRE(ring.begin_frame() != nullptr);
    REQUIRE(ring.publish_frame());
    vr::SharedFp16TextureSnapshot second;
    REQUIRE(ring.acquire_latest(second));
    REQUIRE(second.frame_generation > first.frame_generation);
    REQUIRE(second.buffer_index != first.buffer_index);

    REQUIRE(ring.resize(7, 5));
    REQUIRE(ring.begin_frame() != nullptr);
    REQUIRE(ring.publish_frame());
    vr::SharedFp16TextureSnapshot resized;
    REQUIRE(ring.acquire_latest(resized));
    REQUIRE(resized.ring_generation > first.ring_generation);
    REQUIRE(resized.width == 7);
    REQUIRE(resized.height == 5);

    consume(device1.Get(), ring, first);
    consume(device1.Get(), ring, second);
    consume(device1.Get(), ring, resized);
    REQUIRE(ring.estimated_bytes() ==
            static_cast<uint64_t>(7 * 5 * 8 * 3));
}

TEST_CASE("shared FP16 ring reports backpressure without reusing leases",
          "[windows_dcomp][windows_presentation]") {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = {};
    REQUIRE(SUCCEEDED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, &level, &context)));
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    REQUIRE(SUCCEEDED(device.As(&device1)));

    vr::D3D11SharedFp16Ring ring;
    int callback_count = 0;
    REQUIRE(ring.initialize(device.Get(), context.Get(), 4, 2));
    ring.set_frame_callback([&callback_count]() { ++callback_count; });

    std::array<vr::SharedFp16TextureSnapshot, 3> leases;
    for (auto& lease : leases) {
        REQUIRE(ring.begin_frame() != nullptr);
        REQUIRE(ring.publish_frame());
        REQUIRE(ring.acquire_latest(lease));
    }
    REQUIRE(callback_count == 3);
    REQUIRE(ring.begin_frame() == nullptr);
    REQUIRE(ring.backpressure_count() == 1);

    consume(device1.Get(), ring, leases[0]);
    REQUIRE(ring.begin_frame() != nullptr);
    ring.cancel_frame();
    consume(device1.Get(), ring, leases[1]);
    consume(device1.Get(), ring, leases[2]);
}
