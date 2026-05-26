#include "native_player_bridge.h"

#include "macos/native_player_state.h"

#include <algorithm>
#include <cstdint>
#include <mutex>

using vp_macos::write_error;

void VPMacOSNativePlayerPlay(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->play();
  }
}

void VPMacOSNativePlayerPause(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->pause();
  }
}

void VPMacOSNativePlayerSetSpeed(VPMacOSNativePlayer* player, double speed) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->set_speed(speed);
  }
}

void VPMacOSNativePlayerSetLoopRange(VPMacOSNativePlayer* player,
                                     int enabled,
                                     int64_t start_us,
                                     int64_t end_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->set_loop_range(enabled != 0, start_us, end_us);
  }
}

void VPMacOSNativePlayerSetAudibleTrack(VPMacOSNativePlayer* player,
                                        int32_t file_id) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->set_audible_track(file_id);
  }
}

void VPMacOSNativePlayerSetTrackOffset(VPMacOSNativePlayer* player,
                                       int32_t file_id,
                                       int64_t offset_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->renderer->set_track_offset(file_id, offset_us);
  }
}

int64_t VPMacOSNativePlayerTrackOffsetUs(VPMacOSNativePlayer* player,
                                         int32_t file_id) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked()
      ? player->renderer->track_offset_us(file_id)
      : 0;
}

void VPMacOSNativePlayerSeek(VPMacOSNativePlayer* player, int64_t pts_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (player->renderer_active_locked()) {
    player->clear_last_frame_locked();
    {
      std::lock_guard<std::mutex> callback_lock(player->callback_mutex);
      player->renderer_owned_refresh_min_pts_us =
          std::max<int64_t>(0, pts_us - 500'000);
    }
    player->renderer->seek(pts_us, vr::SeekType::Exact);
  }
}

int VPMacOSNativePlayerStepForward(VPMacOSNativePlayer* player,
                                   char* error,
                                   size_t error_size) {
  if (!player) {
    write_error(error, error_size, "player is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->renderer_active_locked()) {
    write_error(error, error_size, "player is not open");
    return -1;
  }
  player->clear_last_frame_locked();
  player->renderer->step_forward();
  write_error(error, error_size, "");
  return 0;
}

int VPMacOSNativePlayerStepBackward(VPMacOSNativePlayer* player,
                                    char* error,
                                    size_t error_size) {
  if (!player) {
    write_error(error, error_size, "player is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->renderer_active_locked()) {
    write_error(error, error_size, "player is not open");
    return -1;
  }
  player->clear_last_frame_locked();
  player->renderer->step_backward();
  write_error(error, error_size, "");
  return 0;
}

int64_t VPMacOSNativePlayerCurrentPtsUs(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->current_pts_us() : 0;
}

int64_t VPMacOSNativePlayerDurationUs(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->duration_us() : 0;
}

int32_t VPMacOSNativePlayerWidth(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked()
      ? player->renderer->track_dimensions(0).first
      : 0;
}

int32_t VPMacOSNativePlayerHeight(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked()
      ? player->renderer->track_dimensions(0).second
      : 0;
}

int VPMacOSNativePlayerIsPlaying(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() && player->renderer->is_playing() ? 1 : 0;
}

int VPMacOSNativePlayerHasAudio(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() && player->renderer->has_audio() ? 1 : 0;
}

int32_t VPMacOSNativePlayerAudioSampleRate(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->audio_sample_rate() : 0;
}

int32_t VPMacOSNativePlayerAudioChannels(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->audio_channels() : 0;
}

int32_t VPMacOSNativePlayerActiveAudioTrack(VPMacOSNativePlayer* player) {
  if (!player) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->renderer_active_locked() ? player->renderer->audible_track() : -1;
}
