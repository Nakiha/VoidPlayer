#include "video_renderer/exports/ffi_process_globals.h"

#include "common/logging.h"
#include "windows/common/windows_crash_handler.h"
#include "video_renderer/exports/ffi_marshalling.h"
#include "video_renderer/exports/ffi_player_registry.h"

namespace vr::ffi {

naki_vr_status_t configure_logging_process_command(const naki_vr_log_config_t* config) {
    if (!config) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "log config is required");
        return NAKI_VR_ERR_INVALID_ARGUMENT;
    }
    LogConfig cfg;
    if (!to_log_config(*config, cfg)) {
        return g_last_error.status;
    }
    configure_logging(cfg);
    set_ok();
    return NAKI_VR_OK;
}

naki_vr_status_t install_crash_handler_process_command(const char* crash_dir) {
    install_crash_handler(crash_dir ? crash_dir : "");
    set_ok();
    return NAKI_VR_OK;
}

naki_vr_status_t remove_crash_handler_process_command() {
    remove_crash_handler();
    set_ok();
    return NAKI_VR_OK;
}

} // namespace vr::ffi
