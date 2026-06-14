#pragma once

#include "native_diagnostics_ffi.h"
#include "windows/presentation/windows_display_resolver.h"
#include "windows/presentation/windows_presentation_policy.h"

#include <flutter/standard_method_codec.h>

#include <cstdint>
#include <memory>

namespace vr {
class NativePlayer;
}

struct ProcessMemoryUsage {
    uint64_t working_set_bytes = 0;
    uint64_t private_bytes = 0;
};

struct ProcessHeapUsage {
    uint64_t allocated_bytes = 0;
    uint64_t committed_bytes = 0;
    uint64_t reserved_bytes = 0;
    uint32_t heap_count = 0;
};

class NativeDiagnosticsProvider {
public:
    ProcessMemoryUsage QueryProcessMemoryUsage() const;
    ProcessHeapUsage QueryProcessHeapUsage() const;
    uint64_t QueryDedicatedVideoMemoryUsage() const;

    flutter::EncodableMap BuildMethodChannelDiagnostics(
        const std::shared_ptr<vr::NativePlayer>& active_player,
        const vr::WindowsDisplayProbeSnapshot& display,
        const vr::WindowsPresentationPolicy& presentation_policy,
        const std::string& presentation_sdr_white_level_status) const;
    void FillFfiDiagnostics(
        NakiVrDiagnostics& out,
        const std::shared_ptr<vr::NativePlayer>& active_player) const;
};
