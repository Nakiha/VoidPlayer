#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace vr {

enum class WindowsDeviceRecoveryState {
    Stable,
    DeviceLostDetected,
    HoldingLastFrame,
    RebuildingPresentation,
    WaitingForFreshVideo,
    ReactivatingCompositor,
    Recovered,
    FallbackNativeSdr,
    FallbackFlutterTextureSdr,
    FailedTerminal,
};

struct WindowsDeviceRecoveryDiagnostics {
    WindowsDeviceRecoveryState state = WindowsDeviceRecoveryState::Stable;
    uint64_t generation = 0;
    uint64_t attempt_count = 0;
    uint64_t success_count = 0;
    uint64_t failure_count = 0;
    uint64_t last_duration_ms = 0;
    std::string last_reason = "none";
    std::string last_removed_reason = "0x00000000";
    std::string fallback_stage = "none";
    bool preserved_player = true;
    int preserved_track_count = 0;
    bool last_frame_held = false;
};

const char* windows_device_recovery_state_name(
    WindowsDeviceRecoveryState state);

bool windows_hresult_is_device_loss(HRESULT hr);

bool windows_removed_reason_indicates_device_loss(HRESULT hr);

std::string windows_hresult_hex(HRESULT hr);

} // namespace vr
