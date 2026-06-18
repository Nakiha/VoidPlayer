#include <catch2/catch_test_macros.hpp>

#include "renderer/exports/ffi_marshalling.h"
#include "renderer/exports/ffi_player_commands.h"
#include "renderer/exports/ffi_player_lifecycle.h"
#include "renderer/exports/ffi_player_registry.h"

#include <memory>
#include <string>

using namespace vr::ffi;

namespace {

class ScopedFfiPlayer {
public:
    ScopedFfiPlayer() {
        state_ = std::make_shared<PlayerHandleState>();
        handle_ = static_cast<naki_vr_player_t>(state_.get());
        register_player(state_);
    }

    ~ScopedFfiPlayer() {
        auto state = unregister_player(raw_player(handle_));
        if (state) {
            state->player.shutdown();
        }
    }

    naki_vr_player_t handle() const {
        return handle_;
    }

private:
    std::shared_ptr<PlayerHandleState> state_;
    naki_vr_player_t handle_ = nullptr;
};

void init_layout_state(naki_vr_player_layout_state_t& state) {
    state = {};
    state.size = sizeof(state);
    state.abi_version = NAKI_VR_ABI_VERSION;
    state.mode = NAKI_VR_LAYOUT_SIDE_BY_SIDE;
    state.split_pos = 0.5f;
    state.zoom_ratio = 1.0f;
    state.pixel_size_mode = NAKI_VR_PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;
    state.order[0] = 0;
    state.order[1] = 1;
    state.order[2] = 2;
    state.order[3] = 3;
}

} // namespace

TEST_CASE("FfiPlayerCommands: basic playback commands use checked handle leases",
          "[ffi][commands]") {
    ScopedFfiPlayer player;

    REQUIRE(play_player_command(player.handle()) == NAKI_VR_ERR_NOT_INITIALIZED);
    REQUIRE(pause_player_command(player.handle()) == NAKI_VR_ERR_NOT_INITIALIZED);
    REQUIRE(seek_typed_player_command(player.handle(), 1000, NAKI_VR_SEEK_EXACT) ==
            NAKI_VR_ERR_NOT_INITIALIZED);
    REQUIRE(set_player_speed_command(player.handle(), 1.25) ==
            NAKI_VR_ERR_NOT_INITIALIZED);
    REQUIRE(query_player_current_speed(player.handle()) == 1.0);
    REQUIRE(query_player_is_initialized(player.handle()) == 0);
}

TEST_CASE("FfiPlayerCommands: player-scoped command validation preserves outputs",
          "[ffi][commands]") {
    ScopedFfiPlayer player;

    REQUIRE(set_player_speed_command(player.handle(), 0.0) ==
            NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(g_last_error.status == NAKI_VR_ERR_INVALID_ARGUMENT);
    char error[128] = {};
    REQUIRE(copy_player_error_lifecycle_command(player.handle(), error, sizeof(error)) ==
            NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(std::string(error).find("speed") != std::string::npos);

    int slot = 42;
    REQUIRE(add_player_track_command(player.handle(), nullptr, &slot) ==
            NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(slot == -1);
    REQUIRE(copy_player_error_lifecycle_command(player.handle(), error, sizeof(error)) ==
            NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(std::string(error).find("video_path") != std::string::npos);
}

TEST_CASE("FfiPlayerCommands: successful queries clear stale errors",
          "[ffi][commands]") {
    ScopedFfiPlayer player;

    REQUIRE(set_player_speed_command(player.handle(), 0.0) ==
            NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(g_last_error.status == NAKI_VR_ERR_INVALID_ARGUMENT);

    REQUIRE(query_player_track_count(player.handle()) == 0);
    REQUIRE(g_last_error.status == NAKI_VR_OK);

    char error[128] = {};
    REQUIRE(copy_player_error_lifecycle_command(player.handle(), error, sizeof(error)) ==
            NAKI_VR_OK);
    REQUIRE(std::string(error).empty());
}

TEST_CASE("FfiPlayerCommands: layout commands share FFI layout marshalling",
          "[ffi][commands]") {
    ScopedFfiPlayer player;
    naki_vr_player_layout_state_t layout;
    init_layout_state(layout);

    REQUIRE(apply_player_layout_command(player.handle(), &layout) ==
            NAKI_VR_ERR_NOT_INITIALIZED);

    naki_vr_player_layout_state_t out;
    init_layout_state(out);
    query_player_layout_command(player.handle(), &out);
    REQUIRE(g_last_error.status == NAKI_VR_OK);
    REQUIRE(out.size == sizeof(out));
    REQUIRE(out.abi_version == NAKI_VR_ABI_VERSION);
    REQUIRE(out.mode == layout.mode);
    REQUIRE(out.split_pos == layout.split_pos);
}

TEST_CASE("FfiPlayerCommands: invalid handles keep legacy fallback behavior",
          "[ffi][commands]") {
    REQUIRE(play_player_command(nullptr) == NAKI_VR_ERR_INVALID_ARGUMENT);
    REQUIRE(query_player_is_playing(nullptr) == 0);
    REQUIRE(g_last_error.status == NAKI_VR_ERR_INVALID_ARGUMENT);
}
