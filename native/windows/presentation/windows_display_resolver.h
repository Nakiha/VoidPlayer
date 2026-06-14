#pragma once

#include <dxgi1_6.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vr {

struct WindowsDisplayRect {
    int64_t left = 0;
    int64_t top = 0;
    int64_t right = 0;
    int64_t bottom = 0;
};

struct WindowsDisplayOutputCandidate {
    WindowsDisplayRect desktop_rect;
    uint64_t monitor_id = 0;
    uint32_t adapter_index = 0;
    uint32_t output_index = 0;
    bool attached_to_desktop = false;
};

enum class WindowsDisplaySelectionReason {
    None,
    GreatestIntersection,
    NearestMonitor,
    FirstAttachedOutput,
};

struct WindowsDisplaySelection {
    bool resolved = false;
    size_t candidate_index = 0;
    uint64_t intersection_area = 0;
    WindowsDisplaySelectionReason reason = WindowsDisplaySelectionReason::None;
};

WindowsDisplaySelection select_windows_display_output(
    const WindowsDisplayRect& window_rect,
    uint64_t nearest_monitor_id,
    const std::vector<WindowsDisplayOutputCandidate>& candidates);

const char* windows_display_selection_reason_name(
    WindowsDisplaySelectionReason reason);

std::string windows_display_color_space_name(DXGI_COLOR_SPACE_TYPE color_space);
bool windows_display_color_space_is_hdr(DXGI_COLOR_SPACE_TYPE color_space);
std::string windows_display_advanced_color_state(
    bool color_metadata_available,
    DXGI_COLOR_SPACE_TYPE color_space);

struct WindowsDisplayProbeResult {
    std::string status = "unprobed";
    std::string selection_reason = "none";
    std::string device_name;
    std::string adapter_description;
    std::string rotation = "unspecified";
    std::string color_space = "unavailable";
    std::string advanced_color_state = "unavailable";
    int64_t desktop_left = 0;
    int64_t desktop_top = 0;
    int64_t desktop_width = 0;
    int64_t desktop_height = 0;
    int64_t intersection_area = 0;
    int64_t bits_per_color = 0;
    int64_t min_luminance_milli_nits = 0;
    int64_t max_luminance_milli_nits = 0;
    int64_t max_full_frame_luminance_milli_nits = 0;
    int32_t adapter_luid_high = 0;
    uint32_t adapter_luid_low = 0;
    bool output_resolved = false;
    bool color_metadata_available = false;
    bool matches_presentation_adapter = false;
    bool hdr_active = false;
};

class WindowsDisplayResolver {
public:
    WindowsDisplayProbeResult Probe(
        HWND window,
        IDXGIAdapter* presentation_adapter) const;
};

} // namespace vr
