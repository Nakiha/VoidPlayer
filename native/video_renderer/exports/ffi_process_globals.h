#pragma once

#include "video_renderer/exports/ffi_exports.h"

namespace vr::ffi {

naki_vr_status_t configure_logging_process_command(const naki_vr_log_config_t* config);
naki_vr_status_t install_crash_handler_process_command(const char* crash_dir);
naki_vr_status_t remove_crash_handler_process_command();

} // namespace vr::ffi
