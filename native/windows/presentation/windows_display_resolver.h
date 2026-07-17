#pragma once

#include <dxgi1_6.h>
#include <windows.h>

#include <cstdint>
#include <string>

namespace vr {

struct WindowsDisplayProbeResult {
  std::string status = "unprobed";
  std::string device_name;
  std::string adapter_description;
  std::string color_space = "unavailable";
  std::string advanced_color_api = "unavailable";
  std::string sdr_white_level_status = "nominal-default";
  int64_t bits_per_color = 0;
  int64_t sdr_white_level_milli_nits = 80000;
  int32_t adapter_luid_high = 0;
  uint32_t adapter_luid_low = 0;
  bool output_resolved = false;
  bool color_metadata_available = false;
  bool matches_presentation_adapter = false;
  bool hdr_active = false;
  bool advanced_color_supported = false;
  bool advanced_color_active = false;
};

std::string windows_display_color_space_name(DXGI_COLOR_SPACE_TYPE color_space);
bool windows_display_color_space_is_hdr(DXGI_COLOR_SPACE_TYPE color_space);
int64_t windows_sdr_white_level_milli_nits(uint32_t raw_white_level);

class WindowsDisplayResolver {
 public:
  WindowsDisplayProbeResult Probe(HWND window,
                                  IDXGIAdapter* presentation_adapter) const;
};

}  // namespace vr
