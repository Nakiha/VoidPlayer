#include "windows_display_resolver.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

#include <wrl/client.h>

namespace vr {
namespace {

using Microsoft::WRL::ComPtr;

struct EnumeratedOutput {
    WindowsDisplayOutputCandidate candidate;
    ComPtr<IDXGIOutput> output;
    DXGI_ADAPTER_DESC1 adapter_desc = {};
};

std::string luid_string(const LUID& luid) {
    return std::to_string(luid.HighPart) + ":" +
           std::to_string(luid.LowPart);
}

uint64_t intersection_area(
    const WindowsDisplayRect& lhs,
    const WindowsDisplayRect& rhs) {
    const int64_t width =
        std::max<int64_t>(0, std::min(lhs.right, rhs.right) -
                                std::max(lhs.left, rhs.left));
    const int64_t height =
        std::max<int64_t>(0, std::min(lhs.bottom, rhs.bottom) -
                                std::max(lhs.top, rhs.top));
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
}

std::string utf8_from_wide(const wchar_t* value) {
    if (!value || value[0] == L'\0') {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr) <= 0) {
        return {};
    }
    result.pop_back();
    return result;
}

std::string rotation_name(DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
    case DXGI_MODE_ROTATION_IDENTITY:
        return "identity";
    case DXGI_MODE_ROTATION_ROTATE90:
        return "rotate-90";
    case DXGI_MODE_ROTATION_ROTATE180:
        return "rotate-180";
    case DXGI_MODE_ROTATION_ROTATE270:
        return "rotate-270";
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    default:
        return "unspecified";
    }
}

int64_t luminance_to_milli_nits(float value) {
    if (!std::isfinite(value) || value <= 0.0f) {
        return 0;
    }
    constexpr double kMax =
        static_cast<double>(std::numeric_limits<int64_t>::max());
    const double scaled = static_cast<double>(value) * 1000.0;
    return scaled >= kMax ? std::numeric_limits<int64_t>::max()
                          : static_cast<int64_t>(std::llround(scaled));
}

bool luid_equal(const LUID& lhs, const LUID& rhs) {
    return lhs.HighPart == rhs.HighPart && lhs.LowPart == rhs.LowPart;
}

bool query_sdr_white_level(
    const wchar_t* gdi_device_name,
    uint32_t& raw_white_level) {
    raw_white_level = 0;
    if (!gdi_device_name || gdi_device_name[0] == L'\0') {
        return false;
    }

    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG status = GetDisplayConfigBufferSizes(
        QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (status != ERROR_SUCCESS || path_count == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    status = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &path_count,
        paths.data(),
        &mode_count,
        modes.data(),
        nullptr);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    paths.resize(path_count);

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            _wcsicmp(source.viewGdiDeviceName, gdi_device_name) != 0) {
            continue;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS &&
            white.SDRWhiteLevel > 0) {
            raw_white_level = white.SDRWhiteLevel;
            return true;
        }
    }
    return false;
}

std::string advanced_color_mode_name(int mode) {
#ifdef DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2
    switch (mode) {
    case DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR:
        return "sdr";
    case DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG:
        return "wcg";
    case DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR:
        return "hdr";
    default:
        break;
    }
#endif
    return "unknown-" + std::to_string(mode);
}

bool query_advanced_color_info(
    const wchar_t* gdi_device_name,
    WindowsDisplayProbeResult& result) {
    if (!gdi_device_name || gdi_device_name[0] == L'\0') {
        return false;
    }

    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG status = GetDisplayConfigBufferSizes(
        QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (status != ERROR_SUCCESS || path_count == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    status = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &path_count,
        paths.data(),
        &mode_count,
        modes.data(),
        nullptr);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    paths.resize(path_count);

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            _wcsicmp(source.viewGdiDeviceName, gdi_device_name) != 0) {
            continue;
        }

#ifdef DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 info2 = {};
        info2.header.type =
            DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        info2.header.size = sizeof(info2);
        info2.header.adapterId = path.targetInfo.adapterId;
        info2.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
            result.advanced_color_api = "displayconfig-info2";
            result.advanced_color_supported =
                info2.advancedColorSupported != 0;
            result.advanced_color_active =
                info2.advancedColorActive != 0;
            result.advanced_color_limited_by_policy =
                info2.advancedColorLimitedByPolicy != 0;
            result.high_dynamic_range_supported =
                info2.highDynamicRangeSupported != 0;
            result.high_dynamic_range_user_enabled =
                info2.highDynamicRangeUserEnabled != 0;
            result.wide_color_supported = info2.wideColorSupported != 0;
            result.wide_color_user_enabled =
                info2.wideColorUserEnabled != 0;
            result.bits_per_color = info2.bitsPerColorChannel > 0
                ? info2.bitsPerColorChannel
                : result.bits_per_color;
            result.advanced_color_mode =
                advanced_color_mode_name(info2.activeColorMode);
            if (result.advanced_color_active) {
                result.calibration_mode = "advanced-color-system";
                result.calibration_source = "windows-color-system";
            } else {
                result.calibration_mode = "legacy-srgb-assumed";
                result.calibration_source = "none";
            }
            return true;
        }
#endif

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info = {};
        info.header.type =
            DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        info.header.size = sizeof(info);
        info.header.adapterId = path.targetInfo.adapterId;
        info.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) {
            result.advanced_color_api = "displayconfig-info1";
            result.advanced_color_supported =
                info.advancedColorSupported != 0;
            result.advanced_color_active =
                info.advancedColorEnabled != 0;
            result.advanced_color_limited_by_policy =
                info.advancedColorForceDisabled != 0;
            result.wide_color_supported = info.wideColorEnforced != 0;
            result.wide_color_user_enabled = info.wideColorEnforced != 0;
            result.bits_per_color = info.bitsPerColorChannel > 0
                ? info.bitsPerColorChannel
                : result.bits_per_color;
            result.advanced_color_mode =
                result.advanced_color_active ? "advanced-color" : "sdr";
            if (result.advanced_color_active) {
                result.calibration_mode = "advanced-color-system";
                result.calibration_source = "windows-color-system";
            } else {
                result.calibration_mode = "legacy-srgb-assumed";
                result.calibration_source = "none";
            }
            return true;
        }
    }
    return false;
}

bool output_identity_changed(
    const WindowsDisplayProbeResult& previous,
    const WindowsDisplayProbeResult& current) {
    return previous.output_resolved != current.output_resolved ||
           previous.device_name != current.device_name ||
           previous.output_identity != current.output_identity ||
           previous.adapter_luid_high != current.adapter_luid_high ||
           previous.adapter_luid_low != current.adapter_luid_low ||
           previous.matches_presentation_adapter !=
               current.matches_presentation_adapter;
}

bool color_state_changed(
    const WindowsDisplayProbeResult& previous,
    const WindowsDisplayProbeResult& current) {
    return previous.color_metadata_available !=
               current.color_metadata_available ||
           previous.bits_per_color != current.bits_per_color ||
           previous.color_space != current.color_space ||
           previous.advanced_color_state != current.advanced_color_state ||
           previous.hdr_active != current.hdr_active ||
           previous.min_luminance_milli_nits !=
               current.min_luminance_milli_nits ||
           previous.max_luminance_milli_nits !=
               current.max_luminance_milli_nits ||
           previous.max_full_frame_luminance_milli_nits !=
               current.max_full_frame_luminance_milli_nits ||
           previous.sdr_white_level_status != current.sdr_white_level_status ||
           previous.sdr_white_level_milli_nits !=
               current.sdr_white_level_milli_nits ||
           previous.advanced_color_api != current.advanced_color_api ||
           previous.advanced_color_mode != current.advanced_color_mode ||
           previous.calibration_mode != current.calibration_mode ||
           previous.advanced_color_active != current.advanced_color_active ||
           previous.high_dynamic_range_user_enabled !=
               current.high_dynamic_range_user_enabled ||
           previous.wide_color_user_enabled !=
               current.wide_color_user_enabled;
}

bool output_geometry_changed(
    const WindowsDisplayProbeResult& previous,
    const WindowsDisplayProbeResult& current) {
    return previous.desktop_left != current.desktop_left ||
           previous.desktop_top != current.desktop_top ||
           previous.desktop_width != current.desktop_width ||
           previous.desktop_height != current.desktop_height ||
           previous.rotation != current.rotation;
}

} // namespace

WindowsDisplaySelection select_windows_display_output(
    const WindowsDisplayRect& window_rect,
    uint64_t nearest_monitor_id,
    const std::vector<WindowsDisplayOutputCandidate>& candidates) {
    WindowsDisplaySelection result;
    bool have_attached = false;
    size_t first_attached = 0;
    uint64_t best_area = 0;
    size_t best_index = 0;

    for (size_t index = 0; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        if (!candidate.attached_to_desktop) {
            continue;
        }
        if (!have_attached) {
            have_attached = true;
            first_attached = index;
        }
        const uint64_t area =
            intersection_area(window_rect, candidate.desktop_rect);
        if (area > best_area) {
            best_area = area;
            best_index = index;
        }
    }

    if (!have_attached) {
        return result;
    }
    if (best_area > 0) {
        result.resolved = true;
        result.candidate_index = best_index;
        result.intersection_area = best_area;
        result.reason = WindowsDisplaySelectionReason::GreatestIntersection;
        return result;
    }
    if (nearest_monitor_id != 0) {
        for (size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            if (candidate.attached_to_desktop &&
                candidate.monitor_id == nearest_monitor_id) {
                result.resolved = true;
                result.candidate_index = index;
                result.reason = WindowsDisplaySelectionReason::NearestMonitor;
                return result;
            }
        }
    }

    result.resolved = true;
    result.candidate_index = first_attached;
    result.reason = WindowsDisplaySelectionReason::FirstAttachedOutput;
    return result;
}

const char* windows_display_selection_reason_name(
    WindowsDisplaySelectionReason reason) {
    switch (reason) {
    case WindowsDisplaySelectionReason::GreatestIntersection:
        return "greatest-intersection";
    case WindowsDisplaySelectionReason::NearestMonitor:
        return "nearest-monitor";
    case WindowsDisplaySelectionReason::FirstAttachedOutput:
        return "first-attached-output";
    case WindowsDisplaySelectionReason::None:
    default:
        return "none";
    }
}

std::string windows_display_color_space_name(
    DXGI_COLOR_SPACE_TYPE color_space) {
    switch (color_space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return "rgb-full-g22-p709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return "rgb-full-g10-p709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        return "rgb-studio-g22-p709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020:
        return "rgb-studio-g22-p2020";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601:
        return "ycbcr-full-g22-p709-x601";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601:
        return "ycbcr-studio-g22-left-p601";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601:
        return "ycbcr-full-g22-left-p601";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709:
        return "ycbcr-studio-g22-left-p709";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709:
        return "ycbcr-full-g22-left-p709";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020:
        return "ycbcr-studio-g22-left-p2020";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020:
        return "ycbcr-full-g22-left-p2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return "rgb-full-pq-p2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
        return "ycbcr-studio-pq-left-p2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        return "rgb-studio-pq-p2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020:
        return "ycbcr-studio-g22-topleft-p2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        return "ycbcr-studio-pq-topleft-p2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
        return "rgb-full-g22-p2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
        return "ycbcr-studio-hlg-topleft-p2020";
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
        return "ycbcr-full-hlg-topleft-p2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709:
        return "rgb-studio-g24-p709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020:
        return "rgb-studio-g24-p2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709:
        return "ycbcr-studio-g24-left-p709";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020:
        return "ycbcr-studio-g24-left-p2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020:
        return "ycbcr-studio-g24-topleft-p2020";
    case DXGI_COLOR_SPACE_RESERVED:
        return "reserved";
    case DXGI_COLOR_SPACE_CUSTOM:
        return "custom";
    default:
        return "unknown-" +
               std::to_string(static_cast<uint32_t>(color_space));
    }
}

bool windows_display_color_space_is_hdr(
    DXGI_COLOR_SPACE_TYPE color_space) {
    switch (color_space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
        return true;
    default:
        return false;
    }
}

std::string windows_display_advanced_color_state(
    bool color_metadata_available,
    DXGI_COLOR_SPACE_TYPE color_space) {
    if (!color_metadata_available) {
        return "unavailable";
    }
    return windows_display_color_space_is_hdr(color_space)
               ? "hdr-active"
               : "sdr-or-advanced-color-unknown";
}

int64_t windows_sdr_white_level_milli_nits(uint32_t raw_white_level) {
    constexpr uint64_t kMilliNitsPerRawUnit = 80;
    const uint64_t scaled =
        static_cast<uint64_t>(raw_white_level) * kMilliNitsPerRawUnit;
    return scaled > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
        ? std::numeric_limits<int64_t>::max()
        : static_cast<int64_t>(scaled);
}

WindowsDisplayProbeResult WindowsDisplayResolver::Probe(
    HWND window,
    IDXGIAdapter* presentation_adapter) const {
    WindowsDisplayProbeResult result;
    if (!window || !IsWindow(window)) {
        result.status = "invalid-window";
        return result;
    }

    HWND root_window = GetAncestor(window, GA_ROOT);
    if (!root_window) {
        root_window = window;
    }
    RECT native_window_rect = {};
    if (!GetWindowRect(root_window, &native_window_rect)) {
        result.status = "window-rect-failed";
        return result;
    }
    const WindowsDisplayRect window_rect{
        native_window_rect.left,
        native_window_rect.top,
        native_window_rect.right,
        native_window_rect.bottom,
    };
    const HMONITOR nearest_monitor =
        MonitorFromWindow(root_window, MONITOR_DEFAULTTONEAREST);
    const uint64_t nearest_monitor_id =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(nearest_monitor));

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        result.status = "factory-create-failed";
        return result;
    }

    std::vector<EnumeratedOutput> outputs;
    std::vector<WindowsDisplayOutputCandidate> candidates;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapter_hr =
            factory->EnumAdapters1(adapter_index, &adapter);
        if (adapter_hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adapter_hr) || !adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 adapter_desc = {};
        adapter->GetDesc1(&adapter_desc);
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_hr =
                adapter->EnumOutputs(output_index, &output);
            if (output_hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(output_hr) || !output) {
                continue;
            }
            DXGI_OUTPUT_DESC output_desc = {};
            if (FAILED(output->GetDesc(&output_desc))) {
                continue;
            }

            WindowsDisplayOutputCandidate candidate;
            candidate.desktop_rect = {
                output_desc.DesktopCoordinates.left,
                output_desc.DesktopCoordinates.top,
                output_desc.DesktopCoordinates.right,
                output_desc.DesktopCoordinates.bottom,
            };
            candidate.monitor_id = static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(output_desc.Monitor));
            candidate.adapter_index = adapter_index;
            candidate.output_index = output_index;
            candidate.attached_to_desktop =
                output_desc.AttachedToDesktop != FALSE;
            candidates.push_back(candidate);
            outputs.push_back(
                EnumeratedOutput{candidate, std::move(output), adapter_desc});
        }
    }

    const auto selection = select_windows_display_output(
        window_rect, nearest_monitor_id, candidates);
    result.selection_reason =
        windows_display_selection_reason_name(selection.reason);
    if (!selection.resolved || selection.candidate_index >= outputs.size()) {
        result.status = "no-attached-output";
        return result;
    }

    const auto& selected = outputs[selection.candidate_index];
    result.output_resolved = true;
    result.intersection_area =
        static_cast<int64_t>(selection.intersection_area);
    result.adapter_description =
        utf8_from_wide(selected.adapter_desc.Description);
    result.adapter_luid_high = selected.adapter_desc.AdapterLuid.HighPart;
    result.adapter_luid_low = selected.adapter_desc.AdapterLuid.LowPart;
    result.adapter_index = selected.candidate.adapter_index;
    result.output_index = selected.candidate.output_index;
    result.output_identity =
        luid_string(selected.adapter_desc.AdapterLuid) + "/" +
        std::to_string(selected.candidate.output_index) + "/" +
        std::to_string(selected.candidate.monitor_id);
    result.desktop_left = selected.candidate.desktop_rect.left;
    result.desktop_top = selected.candidate.desktop_rect.top;
    result.desktop_width =
        selected.candidate.desktop_rect.right -
        selected.candidate.desktop_rect.left;
    result.desktop_height =
        selected.candidate.desktop_rect.bottom -
        selected.candidate.desktop_rect.top;

    DXGI_ADAPTER_DESC presentation_desc = {};
    if (presentation_adapter &&
        SUCCEEDED(presentation_adapter->GetDesc(&presentation_desc))) {
        result.matches_presentation_adapter = luid_equal(
            selected.adapter_desc.AdapterLuid,
            presentation_desc.AdapterLuid);
    }

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(selected.output.As(&output6)) || !output6) {
        result.status = "output6-unavailable";
        return result;
    }
    DXGI_OUTPUT_DESC1 output_desc1 = {};
    if (FAILED(output6->GetDesc1(&output_desc1))) {
        result.status = "output-desc1-failed";
        return result;
    }

    result.status = "ok";
    result.color_metadata_available = true;
    result.device_name = utf8_from_wide(output_desc1.DeviceName);
    result.rotation = rotation_name(output_desc1.Rotation);
    result.bits_per_color = output_desc1.BitsPerColor;
    result.color_space =
        windows_display_color_space_name(output_desc1.ColorSpace);
    result.hdr_active =
        windows_display_color_space_is_hdr(output_desc1.ColorSpace);
    result.advanced_color_state =
        windows_display_advanced_color_state(true, output_desc1.ColorSpace);
    result.min_luminance_milli_nits =
        luminance_to_milli_nits(output_desc1.MinLuminance);
    result.max_luminance_milli_nits =
        luminance_to_milli_nits(output_desc1.MaxLuminance);
    result.max_full_frame_luminance_milli_nits =
        luminance_to_milli_nits(output_desc1.MaxFullFrameLuminance);
    result.red_primary_x =
        static_cast<int64_t>(std::llround(output_desc1.RedPrimary[0] * 1000000.0f));
    result.red_primary_y =
        static_cast<int64_t>(std::llround(output_desc1.RedPrimary[1] * 1000000.0f));
    result.green_primary_x =
        static_cast<int64_t>(std::llround(output_desc1.GreenPrimary[0] * 1000000.0f));
    result.green_primary_y =
        static_cast<int64_t>(std::llround(output_desc1.GreenPrimary[1] * 1000000.0f));
    result.blue_primary_x =
        static_cast<int64_t>(std::llround(output_desc1.BluePrimary[0] * 1000000.0f));
    result.blue_primary_y =
        static_cast<int64_t>(std::llround(output_desc1.BluePrimary[1] * 1000000.0f));
    result.white_point_x =
        static_cast<int64_t>(std::llround(output_desc1.WhitePoint[0] * 1000000.0f));
    result.white_point_y =
        static_cast<int64_t>(std::llround(output_desc1.WhitePoint[1] * 1000000.0f));
    if (!query_advanced_color_info(output_desc1.DeviceName, result)) {
        result.advanced_color_api = "dxgi-output6";
        result.advanced_color_mode = result.hdr_active ? "hdr" : "unknown";
        result.calibration_mode =
            result.hdr_active ? "advanced-color-system"
                              : "legacy-srgb-assumed";
        result.calibration_source =
            result.hdr_active ? "dxgi-output6" : "none";
    }
    if (result.hdr_active) {
        uint32_t raw_white_level = 0;
        if (query_sdr_white_level(
                output_desc1.DeviceName, raw_white_level)) {
            result.sdr_white_level_status = "queried";
            result.sdr_white_level_milli_nits =
                windows_sdr_white_level_milli_nits(raw_white_level);
        }
    }
    return result;
}

bool WindowsDisplayResolver::OpenAdapterForProbe(
    const WindowsDisplayProbeResult& probe,
    IDXGIAdapter** adapter_out) const {
    if (!adapter_out) {
        return false;
    }
    *adapter_out = nullptr;
    if (!probe.output_resolved) {
        return false;
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            return false;
        }
        if (FAILED(hr) || !adapter) {
            continue;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        if (desc.AdapterLuid.HighPart == probe.adapter_luid_high &&
            desc.AdapterLuid.LowPart == probe.adapter_luid_low) {
            *adapter_out = adapter.Detach();
            return true;
        }
    }
}

WindowsDisplayProbeSnapshot WindowsDisplayProbeTracker::Update(
    const WindowsDisplayProbeResult& probe) {
    std::lock_guard lock(mutex_);
    if (current_.generation == 0) {
        current_.probe = probe;
        current_.generation = 1;
        current_.last_change_reason = "initial-probe";
        current_.changed = true;
        return current_;
    }

    std::string change_reason;
    if (current_.probe.status != probe.status) {
        change_reason = "probe-status-changed";
    } else if (output_identity_changed(current_.probe, probe)) {
        change_reason = "output-changed";
    } else if (color_state_changed(current_.probe, probe)) {
        change_reason = "color-state-changed";
    } else if (output_geometry_changed(current_.probe, probe)) {
        change_reason = "output-geometry-changed";
    }

    current_.probe = probe;
    current_.changed = !change_reason.empty();
    if (current_.changed) {
        ++current_.generation;
        ++current_.change_count;
        current_.last_change_reason = std::move(change_reason);
    }
    return current_;
}

} // namespace vr
