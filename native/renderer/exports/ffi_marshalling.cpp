#include "renderer/exports/ffi_marshalling.h"

#include "renderer/exports/ffi_player_registry.h"
#include "renderer/layout/layout_validation.h"
#include "renderer/renderer_config_validation.h"
#include "renderer/renderer_limits.h"

#include <spdlog/common.h>

#include <string>

namespace vr::ffi {
namespace {

bool validate_log_config(const naki_vr_log_config_t& c) {
    if (!validate_abi(c.size, c.abi_version, sizeof(naki_vr_log_config_t), "log config")) {
        return false;
    }
    if (c.level < NAKI_VR_LOG_TRACE || c.level > NAKI_VR_LOG_OFF) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "log level out of range");
        return false;
    }
    if (c.max_files < 0) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "log max_files must be non-negative");
        return false;
    }
    return true;
}

bool validate_player_config(const naki_vr_player_config_t& c) {
    if (!validate_abi(c.size, c.abi_version, sizeof(naki_vr_player_config_t), "player config")) {
        return false;
    }
    if (auto result = validate_renderer_dimensions(c.width, c.height, "player dimensions");
        !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    return validate_log_config(c.log_config);
}

bool validate_player_config_v2(const naki_vr_player_config_v2_t& c) {
    if (!validate_abi(c.size, c.abi_version, sizeof(naki_vr_player_config_v2_t),
                      "player config v2")) {
        return false;
    }
    if (c.flags != 0 || c.reserved0 != 0) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "player config v2 reserved fields must be zero");
        return false;
    }
    for (uint64_t value : c.reserved) {
        if (value != 0) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT,
                      "player config v2 reserved fields must be zero");
            return false;
        }
    }
    if (auto result = validate_renderer_dimensions(c.width, c.height, "player dimensions");
        !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    if (c.video_path_count > 0 && !c.video_paths) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT,
                  "video_paths is required when video_path_count is non-zero");
        return false;
    }
    if (c.video_path_count > kMaxRendererVideoPaths) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "too many video paths");
        return false;
    }
    return validate_log_config(c.log_config);
}

} // namespace

bool validate_abi(uint32_t size, uint32_t version, size_t expected, const char* name) {
    if (size < expected) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, std::string(name) + " size is too small");
        return false;
    }
    if (version != NAKI_VR_ABI_VERSION) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, std::string(name) + " ABI version mismatch");
        return false;
    }
    return true;
}

bool to_log_config(const naki_vr_log_config_t& c, LogConfig& cfg) {
    if (!validate_log_config(c)) {
        return false;
    }
    cfg.pattern = c.pattern ? c.pattern : "";
    cfg.file_path = c.file_path ? c.file_path : "";
    cfg.max_file_size = c.max_file_size;
    cfg.max_files = c.max_files;
    cfg.level = static_cast<spdlog::level::level_enum>(c.level);
    return true;
}

bool fill_renderer_config_v1(const naki_vr_player_config_t& c, RendererConfig& cfg) {
    if (!validate_player_config(c)) {
        return false;
    }
    cfg.hwnd = reinterpret_cast<void*>(c.hwnd);
    cfg.width = c.width;
    cfg.height = c.height;
    cfg.use_hardware_decode = c.use_hardware_decode != 0;
    if (!to_log_config(c.log_config, cfg.log_config)) {
        return false;
    }

    if (c.video_paths) {
        size_t path_count = 0;
        for (; path_count <= kMaxRendererVideoPaths; ++path_count) {
            const char* path = c.video_paths[path_count];
            if (!path) {
                break;
            }
            cfg.video_paths.emplace_back(path);
        }
        if (path_count > kMaxRendererVideoPaths) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT,
                      "too many video paths or missing null terminator");
            return false;
        }
    }

    if (auto result = validate_renderer_config(cfg); !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    return true;
}

bool fill_renderer_config_v2(const naki_vr_player_config_v2_t& c, RendererConfig& cfg) {
    if (!validate_player_config_v2(c)) {
        return false;
    }
    cfg.hwnd = reinterpret_cast<void*>(c.hwnd);
    cfg.width = c.width;
    cfg.height = c.height;
    cfg.use_hardware_decode = c.use_hardware_decode != 0;
    if (!to_log_config(c.log_config, cfg.log_config)) {
        return false;
    }
    for (size_t i = 0; i < c.video_path_count; ++i) {
        const char* path = c.video_paths[i];
        if (!path) {
            set_error(NAKI_VR_ERR_INVALID_ARGUMENT, "video path must not be null");
            return false;
        }
        cfg.video_paths.emplace_back(path);
    }
    if (auto result = validate_renderer_config(cfg); !result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    return true;
}

bool is_valid_seek_type(int type) {
    return type == NAKI_VR_SEEK_KEYFRAME || type == NAKI_VR_SEEK_EXACT;
}

SeekType to_seek_type(int type) {
    return type == NAKI_VR_SEEK_EXACT ? SeekType::Exact : SeekType::Keyframe;
}

LayoutState to_layout_state(const naki_vr_player_layout_state_t& state) {
    LayoutState layout;
    layout.mode = state.mode;
    layout.split_pos = state.split_pos;
    layout.zoom_ratio = state.zoom_ratio;
    layout.view_offset[0] = state.view_offset[0];
    layout.view_offset[1] = state.view_offset[1];
    layout.pixel_size_mode = state.pixel_size_mode;
    for (int i = 0; i < 4; ++i) layout.order[i] = state.order[i];
    return layout;
}

bool validate_ffi_layout_state(const naki_vr_player_layout_state_t& state) {
    if (!validate_abi(state.size,
                      state.abi_version,
                      sizeof(naki_vr_player_layout_state_t),
                      "layout state")) {
        return false;
    }
    const auto result = validate_layout_state(to_layout_state(state));
    if (!result.ok) {
        set_error(NAKI_VR_ERR_INVALID_ARGUMENT, result.message);
        return false;
    }
    return true;
}

void fill_ffi_layout_state(const LayoutState& layout, naki_vr_player_layout_state_t& out_state) {
    out_state.size = sizeof(naki_vr_player_layout_state_t);
    out_state.abi_version = NAKI_VR_ABI_VERSION;
    out_state.mode = layout.mode;
    out_state.split_pos = layout.split_pos;
    out_state.zoom_ratio = layout.zoom_ratio;
    out_state.view_offset[0] = layout.view_offset[0];
    out_state.view_offset[1] = layout.view_offset[1];
    out_state.pixel_size_mode = layout.pixel_size_mode;
    for (int i = 0; i < 4; ++i) out_state.order[i] = layout.order[i];
}

} // namespace vr::ffi
