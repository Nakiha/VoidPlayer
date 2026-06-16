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

TEST_CASE("Windows cross-adapter sync request parser is deterministic",
          "[windows_cross_adapter][windows_dcomp]") {
    REQUIRE(vr::parse_windows_cross_adapter_sync_request(nullptr) ==
            vr::WindowsCrossAdapterSyncRequest::Auto);
    REQUIRE(vr::parse_windows_cross_adapter_sync_request("") ==
            vr::WindowsCrossAdapterSyncRequest::Auto);
    REQUIRE(vr::parse_windows_cross_adapter_sync_request("auto") ==
            vr::WindowsCrossAdapterSyncRequest::Auto);
    REQUIRE(vr::parse_windows_cross_adapter_sync_request("event-query") ==
            vr::WindowsCrossAdapterSyncRequest::EventQuery);
    REQUIRE(vr::parse_windows_cross_adapter_sync_request("shared-fence") ==
            vr::WindowsCrossAdapterSyncRequest::SharedFence);
    REQUIRE(vr::parse_windows_cross_adapter_sync_request("unknown") ==
            vr::WindowsCrossAdapterSyncRequest::Auto);
    REQUIRE(std::string(vr::windows_cross_adapter_sync_request_name(
                vr::WindowsCrossAdapterSyncRequest::SharedFence)) ==
            "shared-fence");
}

TEST_CASE("Windows cross-adapter transport probe reports deterministic status",
          "[windows_cross_adapter][windows_dcomp]") {
    auto missing = vr::probe_windows_cross_adapter_transport(nullptr, nullptr);
    REQUIRE_FALSE(missing.bgra8);
    REQUIRE_FALSE(missing.rgba16f);
    REQUIRE_FALSE(missing.shared_fence_producer);
    REQUIRE_FALSE(missing.shared_fence_output);
    REQUIRE_FALSE(missing.shared_fence_handle_created);
    REQUIRE_FALSE(missing.shared_fence_open_succeeded);
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
    if (support.shared_fence) {
        REQUIRE(support.shared_fence_producer);
        REQUIRE(support.shared_fence_output);
        REQUIRE(support.shared_fence_handle_created);
        REQUIRE(support.shared_fence_open_succeeded);
    }
    if (support.bgra8) {
        REQUIRE(support.row_major);
        REQUIRE(support.status == "ok");
    }
}

TEST_CASE("Windows cross-adapter transport forced shared-fence falls back safely",
          "[windows_cross_adapter][windows_dcomp]") {
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
    if (FAILED(hr) || !device || !context) {
        WARN("Hardware D3D11 device unavailable; forced sync fallback covered by parser/probe tests");
        return;
    }

    const auto support = vr::probe_windows_cross_adapter_transport(
        device.Get(), device.Get());
    if (!support.bgra8) {
        WARN("Row-major shared texture transport unavailable on this adapter");
        return;
    }

    vr::D3D11CrossAdapterTextureTransport transport;
    REQUIRE(transport.initialize(
        device.Get(),
        context.Get(),
        device.Get(),
        context.Get(),
        DXGI_FORMAT_B8G8R8A8_UNORM,
        16,
        16,
        vr::WindowsCrossAdapterSyncRequest::SharedFence));
    REQUIRE(transport.requested_sync_kind() == "shared-fence");
    if (support.shared_fence) {
        REQUIRE(transport.active_sync_kind() == "shared-fence");
        REQUIRE(transport.sync_fallback_reason() == "none");
        REQUIRE(transport.shared_fence_handle_created());
        REQUIRE(transport.shared_fence_open_succeeded());
    } else {
        REQUIRE(transport.active_sync_kind() == "event-query");
        REQUIRE(transport.sync_fallback_reason() != "none");
    }
}
