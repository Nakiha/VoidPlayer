#include "windows/presentation/windows_device_recovery.h"

#include <dxgi.h>

#include <iomanip>
#include <sstream>

namespace vr {

const char* windows_device_recovery_state_name(
    WindowsDeviceRecoveryState state) {
    switch (state) {
    case WindowsDeviceRecoveryState::Stable:
        return "stable";
    case WindowsDeviceRecoveryState::DeviceLostDetected:
        return "device-lost-detected";
    case WindowsDeviceRecoveryState::HoldingLastFrame:
        return "holding-last-frame";
    case WindowsDeviceRecoveryState::RebuildingPresentation:
        return "rebuilding-presentation";
    case WindowsDeviceRecoveryState::WaitingForFreshVideo:
        return "waiting-for-fresh-video";
    case WindowsDeviceRecoveryState::ReactivatingCompositor:
        return "reactivating-compositor";
    case WindowsDeviceRecoveryState::Recovered:
        return "recovered";
    case WindowsDeviceRecoveryState::FallbackNativeSdr:
        return "fallback-native-sdr";
    case WindowsDeviceRecoveryState::FallbackFlutterTextureSdr:
        return "fallback-flutter-texture-sdr";
    case WindowsDeviceRecoveryState::FailedTerminal:
        return "failed-terminal";
    default:
        return "stable";
    }
}

bool windows_hresult_is_device_loss(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED ||
           hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG;
}

bool windows_removed_reason_indicates_device_loss(HRESULT hr) {
    return hr != S_OK;
}

std::string windows_hresult_hex(HRESULT hr) {
    std::ostringstream out;
    out << "0x"
        << std::uppercase
        << std::hex
        << std::setw(8)
        << std::setfill('0')
        << static_cast<unsigned long>(hr);
    return out.str();
}

} // namespace vr
