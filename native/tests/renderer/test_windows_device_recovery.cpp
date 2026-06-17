#include <catch2/catch_test_macros.hpp>

#include <windows.h>
#include <dxgi.h>

#include <string>

#include "windows/presentation/windows_device_recovery.h"

TEST_CASE("Windows device recovery state names are stable",
          "[windows_device_recovery]") {
    using vr::WindowsDeviceRecoveryState;

    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                WindowsDeviceRecoveryState::Stable)) == "stable");
    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                WindowsDeviceRecoveryState::DeviceLostDetected)) ==
            "device-lost-detected");
    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                WindowsDeviceRecoveryState::HoldingLastFrame)) ==
            "holding-last-frame");
    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                WindowsDeviceRecoveryState::RebuildingPresentation)) ==
            "rebuilding-presentation");
    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                WindowsDeviceRecoveryState::WaitingForFreshVideo)) ==
            "waiting-for-fresh-video");
    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                WindowsDeviceRecoveryState::ReactivatingCompositor)) ==
            "reactivating-compositor");
    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                WindowsDeviceRecoveryState::Recovered)) == "recovered");
    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                WindowsDeviceRecoveryState::FallbackNativeSdr)) ==
            "fallback-native-sdr");
    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                WindowsDeviceRecoveryState::FailedTerminal)) ==
            "failed-terminal");
}

TEST_CASE("Windows device recovery classifies device loss narrowly",
          "[windows_device_recovery]") {
    REQUIRE(vr::windows_hresult_is_device_loss(DXGI_ERROR_DEVICE_REMOVED));
    REQUIRE(vr::windows_hresult_is_device_loss(DXGI_ERROR_DEVICE_RESET));
    REQUIRE(vr::windows_hresult_is_device_loss(DXGI_ERROR_DEVICE_HUNG));

    REQUIRE_FALSE(vr::windows_hresult_is_device_loss(E_FAIL));
    REQUIRE_FALSE(vr::windows_hresult_is_device_loss(S_OK));

    REQUIRE(vr::windows_removed_reason_indicates_device_loss(E_FAIL));
    REQUIRE(vr::windows_removed_reason_indicates_device_loss(
        DXGI_ERROR_DEVICE_REMOVED));
    REQUIRE_FALSE(vr::windows_removed_reason_indicates_device_loss(S_OK));
}

TEST_CASE("Windows device recovery diagnostics preserve player contract",
          "[windows_device_recovery]") {
    vr::WindowsDeviceRecoveryDiagnostics diagnostics;
    diagnostics.state = vr::WindowsDeviceRecoveryState::Recovered;
    diagnostics.generation = 3;
    diagnostics.attempt_count = 2;
    diagnostics.success_count = 1;
    diagnostics.preserved_player = true;
    diagnostics.preserved_track_count = 2;
    diagnostics.last_frame_held = true;

    REQUIRE(std::string(vr::windows_device_recovery_state_name(
                diagnostics.state)) == "recovered");
    REQUIRE(diagnostics.generation == 3);
    REQUIRE(diagnostics.attempt_count == 2);
    REQUIRE(diagnostics.success_count == 1);
    REQUIRE(diagnostics.preserved_player);
    REQUIRE(diagnostics.preserved_track_count == 2);
    REQUIRE(diagnostics.last_frame_held);
}
