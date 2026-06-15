#include <catch2/catch_test_macros.hpp>

#include "windows/d3d11/cross_adapter_transport.h"

#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

TEST_CASE("Windows cross-adapter LUID helpers classify same and different adapters",
          "[windows_cross_adapter][windows_dcomp]") {
    REQUIRE(vr::windows_luid_equal(1, 2, 1, 2));
    REQUIRE_FALSE(vr::windows_luid_equal(1, 2, 1, 3));
    REQUIRE_FALSE(vr::windows_cross_adapter_required(1, 2, 1, 2));
    REQUIRE(vr::windows_cross_adapter_required(1, 2, 9, 2));
}

TEST_CASE("Windows cross-adapter transport probe reports deterministic status",
          "[windows_cross_adapter][windows_dcomp]") {
    auto missing = vr::probe_windows_cross_adapter_transport(nullptr, nullptr);
    REQUIRE_FALSE(missing.bgra8);
    REQUIRE_FALSE(missing.rgba16f);
    REQUIRE(missing.status == "missing-device");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level = {};
    const HRESULT hr = D3D11CreateDevice(
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
    if (FAILED(hr) || !device) {
        WARN("Hardware D3D11 device unavailable; transport capability probe covered by missing-device case");
        return;
    }

    const auto support = vr::probe_windows_cross_adapter_transport(
        device.Get(), device.Get());
    REQUIRE_FALSE(support.status.empty());
    REQUIRE_FALSE(support.sync_kind.empty());
    if (support.bgra8) {
        REQUIRE(support.row_major);
        REQUIRE(support.status == "ok");
    }
}
