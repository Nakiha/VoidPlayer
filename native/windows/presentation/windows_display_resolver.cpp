#include "windows_display_resolver.h"

#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <limits>
#include <vector>

namespace vr {
namespace {

using Microsoft::WRL::ComPtr;

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
  if (WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr,
                          nullptr) <= 0) {
    return {};
  }
  result.pop_back();
  return result;
}

bool luid_equal(const LUID& lhs, const LUID& rhs) {
  return lhs.HighPart == rhs.HighPart && lhs.LowPart == rhs.LowPart;
}

bool find_active_display_path(const wchar_t* gdi_device_name,
                              DISPLAYCONFIG_PATH_INFO& selected) {
  if (!gdi_device_name || gdi_device_name[0] == L'\0') {
    return false;
  }
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count,
                                            &mode_count);
  if (status != ERROR_SUCCESS || path_count == 0) {
    return false;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                              &mode_count, modes.data(), nullptr);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  for (UINT32 index = 0; index < path_count; ++index) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[index].sourceInfo.adapterId;
    source.header.id = paths[index].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
        _wcsicmp(source.viewGdiDeviceName, gdi_device_name) == 0) {
      selected = paths[index];
      return true;
    }
  }
  return false;
}

void query_display_config_color(const wchar_t* gdi_device_name,
                                WindowsDisplayProbeResult& result) {
  DISPLAYCONFIG_PATH_INFO path = {};
  if (!find_active_display_path(gdi_device_name, path)) {
    return;
  }
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info = {};
  info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  info.header.size = sizeof(info);
  info.header.adapterId = path.targetInfo.adapterId;
  info.header.id = path.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) {
    result.advanced_color_api = "displayconfig-info1";
    result.advanced_color_supported = info.advancedColorSupported != 0;
    result.advanced_color_active = info.advancedColorEnabled != 0;
    if (info.bitsPerColorChannel > 0) {
      result.bits_per_color = info.bitsPerColorChannel;
    }
  }
  if (!result.hdr_active) {
    return;
  }
  DISPLAYCONFIG_SDR_WHITE_LEVEL white = {};
  white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
  white.header.size = sizeof(white);
  white.header.adapterId = path.targetInfo.adapterId;
  white.header.id = path.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS &&
      white.SDRWhiteLevel > 0) {
    result.sdr_white_level_status = "queried";
    result.sdr_white_level_milli_nits =
        windows_sdr_white_level_milli_nits(white.SDRWhiteLevel);
  }
}

}  // namespace

std::string windows_display_color_space_name(
    DXGI_COLOR_SPACE_TYPE color_space) {
  switch (color_space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
      return "rgb-full-g22-p709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
      return "rgb-full-g10-p709";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
      return "rgb-full-pq-p2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
      return "rgb-studio-pq-p2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
      return "ycbcr-studio-pq-left-p2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
      return "ycbcr-studio-pq-topleft-p2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
      return "ycbcr-studio-hlg-topleft-p2020";
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
      return "ycbcr-full-hlg-topleft-p2020";
    default:
      return "unknown-" + std::to_string(static_cast<uint32_t>(color_space));
  }
}

bool windows_display_color_space_is_hdr(DXGI_COLOR_SPACE_TYPE color_space) {
  switch (color_space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
      return true;
    default:
      return false;
  }
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
    HWND window, IDXGIAdapter* presentation_adapter) const {
  WindowsDisplayProbeResult result;
  if (!window || !IsWindow(window)) {
    result.status = "invalid-window";
    return result;
  }
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  if (!monitor) {
    result.status = "monitor-unavailable";
    return result;
  }
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    result.status = "factory-create-failed";
    return result;
  }
  DXGI_ADAPTER_DESC presentation_desc = {};
  const bool have_presentation_desc =
      presentation_adapter &&
      SUCCEEDED(presentation_adapter->GetDesc(&presentation_desc));
  for (UINT adapter_index = 0;; ++adapter_index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT adapter_hr = factory->EnumAdapters1(adapter_index, &adapter);
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
      const HRESULT output_hr = adapter->EnumOutputs(output_index, &output);
      if (output_hr == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      if (FAILED(output_hr) || !output) {
        continue;
      }
      DXGI_OUTPUT_DESC output_desc = {};
      if (FAILED(output->GetDesc(&output_desc)) ||
          output_desc.Monitor != monitor) {
        continue;
      }
      result.output_resolved = true;
      result.adapter_description = utf8_from_wide(adapter_desc.Description);
      result.adapter_luid_high = adapter_desc.AdapterLuid.HighPart;
      result.adapter_luid_low = adapter_desc.AdapterLuid.LowPart;
      result.matches_presentation_adapter =
          have_presentation_desc &&
          luid_equal(adapter_desc.AdapterLuid, presentation_desc.AdapterLuid);
      ComPtr<IDXGIOutput6> output6;
      if (FAILED(output.As(&output6)) || !output6) {
        result.status = "output6-unavailable";
        return result;
      }
      DXGI_OUTPUT_DESC1 desc1 = {};
      if (FAILED(output6->GetDesc1(&desc1))) {
        result.status = "output-desc1-failed";
        return result;
      }
      result.status = "ok";
      result.color_metadata_available = true;
      result.device_name = utf8_from_wide(desc1.DeviceName);
      result.bits_per_color = desc1.BitsPerColor;
      result.color_space = windows_display_color_space_name(desc1.ColorSpace);
      result.hdr_active = windows_display_color_space_is_hdr(desc1.ColorSpace);
      query_display_config_color(desc1.DeviceName, result);
      return result;
    }
  }
  result.status = "matching-output-unavailable";
  return result;
}

}  // namespace vr
