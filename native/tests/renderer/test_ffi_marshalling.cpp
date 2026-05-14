#include <catch2/catch_test_macros.hpp>

#include "video_renderer/exports/ffi_marshalling.h"
#include "video_renderer/exports/ffi_player_registry.h"

using namespace vr;
using namespace vr::ffi;

namespace {

void init_log_config(naki_vr_log_config_t& cfg) {
    cfg = {};
    cfg.size = sizeof(cfg);
    cfg.abi_version = NAKI_VR_ABI_VERSION;
    cfg.level = NAKI_VR_LOG_INFO;
}

void init_player_config_v2(naki_vr_player_config_v2_t& cfg) {
    cfg = {};
    cfg.size = sizeof(cfg);
    cfg.abi_version = NAKI_VR_ABI_VERSION;
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.hwnd = 1;
    cfg.use_hardware_decode = 1;
    init_log_config(cfg.log_config);
}

void init_layout_state(naki_vr_player_layout_state_t& state) {
    state = {};
    state.size = sizeof(state);
    state.abi_version = NAKI_VR_ABI_VERSION;
    state.mode = NAKI_VR_LAYOUT_SIDE_BY_SIDE;
    state.split_pos = 0.5f;
    state.zoom_ratio = 1.25f;
    state.view_offset[0] = 4.0f;
    state.view_offset[1] = -2.0f;
    state.pixel_size_mode = NAKI_VR_PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;
    state.order[0] = 0;
    state.order[1] = 1;
    state.order[2] = 2;
    state.order[3] = 3;
}

} // namespace

TEST_CASE("FfiMarshalling: log config validates ABI and copies strings",
          "[ffi][marshalling]") {
    naki_vr_log_config_t input;
    init_log_config(input);
    input.pattern = "[%l] %v";
    input.file_path = "native.log";
    input.max_file_size = 4096;
    input.max_files = 2;
    input.level = NAKI_VR_LOG_WARN;

    LogConfig output;
    REQUIRE(to_log_config(input, output));
    REQUIRE(output.pattern == "[%l] %v");
    REQUIRE(output.file_path == "native.log");
    REQUIRE(output.max_file_size == 4096);
    REQUIRE(output.max_files == 2);
    REQUIRE(output.level == spdlog::level::warn);

    input.level = 999;
    REQUIRE_FALSE(to_log_config(input, output));
    REQUIRE(g_last_error.status == NAKI_VR_ERR_INVALID_ARGUMENT);
}

TEST_CASE("FfiMarshalling: player config v2 uses counted paths",
          "[ffi][marshalling]") {
    const char* paths[] = {"C:/videos/a.mp4", "C:/videos/b.mp4"};
    naki_vr_player_config_v2_t input;
    init_player_config_v2(input);
    input.video_paths = paths;
    input.video_path_count = 2;

    RendererConfig output;
    REQUIRE(fill_renderer_config_v2(input, output));
    REQUIRE(output.video_paths.size() == 2);
    REQUIRE(output.video_paths[0] == "C:/videos/a.mp4");
    REQUIRE(output.video_paths[1] == "C:/videos/b.mp4");
    REQUIRE(output.hwnd == reinterpret_cast<void*>(1));
    REQUIRE(output.width == 1920);
    REQUIRE(output.height == 1080);
    REQUIRE(output.use_hardware_decode);
}

TEST_CASE("FfiMarshalling: counted path arrays reject missing storage",
          "[ffi][marshalling]") {
    naki_vr_player_config_v2_t input;
    init_player_config_v2(input);
    input.video_paths = nullptr;
    input.video_path_count = 1;

    RendererConfig output;
    REQUIRE_FALSE(fill_renderer_config_v2(input, output));
    REQUIRE(g_last_error.status == NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(g_last_error.message.find("video_paths is required") != std::string::npos);
}

TEST_CASE("FfiMarshalling: layout state converts both directions",
          "[ffi][marshalling]") {
    naki_vr_player_layout_state_t input;
    init_layout_state(input);

    REQUIRE(validate_ffi_layout_state(input));
    LayoutState layout = to_layout_state(input);
    REQUIRE(layout.mode == input.mode);
    REQUIRE(layout.zoom_ratio == input.zoom_ratio);
    REQUIRE(layout.view_offset[0] == input.view_offset[0]);
    REQUIRE(layout.order[3] == input.order[3]);

    naki_vr_player_layout_state_t output = {};
    fill_ffi_layout_state(layout, output);
    REQUIRE(output.size == sizeof(output));
    REQUIRE(output.abi_version == NAKI_VR_ABI_VERSION);
    REQUIRE(output.mode == input.mode);
    REQUIRE(output.split_pos == input.split_pos);
    REQUIRE(output.zoom_ratio == input.zoom_ratio);
    REQUIRE(output.view_offset[1] == input.view_offset[1]);
    REQUIRE(output.order[2] == input.order[2]);
}

TEST_CASE("FfiMarshalling: seek type validates ABI enum values",
          "[ffi][marshalling]") {
    REQUIRE(is_valid_seek_type(NAKI_VR_SEEK_KEYFRAME));
    REQUIRE(is_valid_seek_type(NAKI_VR_SEEK_EXACT));
    REQUIRE_FALSE(is_valid_seek_type(999));
    REQUIRE(to_seek_type(NAKI_VR_SEEK_KEYFRAME) == SeekType::Keyframe);
    REQUIRE(to_seek_type(NAKI_VR_SEEK_EXACT) == SeekType::Exact);
}
