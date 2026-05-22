#include "native_player_bridge.h"

#include "macos/presentation_adapter.h"
#include "audio/audio_output.h"
#include "audio/audio_output_factory.h"
#include "media/demux_thread.h"
#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include "playback/playback_controller.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/decode/decode_thread.h"
#include "video_renderer/capture/bgra_capture_metrics.h"
#include "video_renderer/seek/seek_coordinator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>

namespace {

void write_error(char* error, size_t error_size, const std::string& message) {
  if (!error || error_size == 0) {
    return;
  }
  const size_t copy_size = std::min(error_size - 1, message.size());
  std::memcpy(error, message.data(), copy_size);
  error[copy_size] = '\0';
}

class MacOSNativePlayerCore {
public:
  MacOSNativePlayerCore()
      : playback_(vr::create_default_audio_output) {}

  ~MacOSNativePlayerCore() { close(); }

  bool open(const char* path, std::string& error) {
    close();
    if (!path || std::strlen(path) == 0) {
      error = "path is empty";
      return false;
    }

    path_ = path;
    seek_controller_ = std::make_unique<vr::SeekController>();
    packet_queue_ = std::make_unique<vr::PacketQueue>(96);
    audio_packet_queue_ = std::make_unique<vr::PacketQueue>(96);
    track_buffer_ = std::make_unique<vr::TrackBuffer>(16, 4);
    demux_ = std::make_unique<vr::DemuxThread>(path_, *packet_queue_, *seek_controller_);
    demux_->add_optional_output(vr::DemuxStreamKind::Audio, *audio_packet_queue_);

    if (!demux_->open()) {
      error = "failed to open demux input";
      close();
      return false;
    }

    const auto& stats = demux_->stats();
    if (!stats.codec_params || stats.video_stream_index < 0 || stats.time_base.den == 0) {
      error = "input has no usable video stream";
      close();
      return false;
    }

    width_ = stats.width;
    height_ = stats.height;
    duration_us_ = stats.duration_us;
    audio_sample_rate_ = stats.sample_rate;
    audio_channels_ = stats.channels;

    playback_.stop_session();
    if (stats.audio_stream_index >= 0 && stats.audio_codec_params) {
      playback_.start_session();
      auto* audio = playback_.audio_output();
      audio_available_ = audio &&
          audio->add_track(0, *audio_packet_queue_, stats.audio_codec_params,
                           stats.audio_time_base);
      if (audio_available_) {
        audio->set_active_track(0);
        audio->set_all_decode_paused(true);
      } else {
        playback_.stop_session();
      }
    }

    decoder_ = std::make_unique<vr::DecodeThread>(
        *packet_queue_, *track_buffer_, stats.codec_params, stats.time_base);
    if (!decoder_->is_valid()) {
      error = "decode thread failed to initialize";
      close();
      return false;
    }
    demux_->set_seek_callback([this](int64_t pts_us, vr::SeekType type) {
      if (decoder_) {
        decoder_->notify_seek(pts_us, type);
      }
      if (auto* audio = playback_.audio_output()) {
        audio->notify_seek(0, pts_us, type);
      }
    });
    if (!decoder_->start()) {
      error = "decode thread failed to start";
      close();
      return false;
    }
    if (!demux_->start_thread()) {
      error = "demux thread failed to start";
      close();
      return false;
    }

    playback_.seek_clock(0);
    playback_.pause();
    playing_ = false;
    last_tick_frame_pts_us_ = std::numeric_limits<int64_t>::min();
    return true;
  }

  void close() {
    playing_ = false;
    playback_.stop_session();
    if (decoder_) {
      decoder_->stop();
    }
    if (demux_) {
      demux_->stop();
    }
    decoder_.reset();
    demux_.reset();
    track_buffer_.reset();
    audio_packet_queue_.reset();
    packet_queue_.reset();
    seek_controller_.reset();
    path_.clear();
    width_ = 0;
    height_ = 0;
    duration_us_ = 0;
    audio_available_ = false;
    audio_sample_rate_ = 0;
    audio_channels_ = 0;
    loop_range_ = vr::LoopRangeState();
    last_tick_frame_pts_us_ = std::numeric_limits<int64_t>::min();
  }

  void play() {
    if (!decoder_) {
      return;
    }
    playing_ = true;
    playback_.play();
  }

  void pause() {
    playing_ = false;
    playback_.pause();
  }

  void set_speed(double speed) {
    playback_.set_speed(std::max(0.01, speed));
  }

  void set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    const auto next = vr::normalize_loop_range_state(enabled, start_us, end_us);
    if (vr::loop_range_states_equal(loop_range_, next)) {
      return;
    }
    loop_range_ = next;
  }

  void set_audible_track(int32_t file_id) {
    if (auto* audio = playback_.audio_output()) {
      audio->set_active_track(file_id);
    }
  }

  void seek(int64_t pts_us) {
    if (!seek_controller_ || !track_buffer_) {
      return;
    }
    const int64_t target = std::max<int64_t>(0, pts_us);
    track_buffer_->set_state(vr::TrackState::Flushing);
    track_buffer_->clear_frames();
    packet_queue_->flush();
    if (audio_packet_queue_) {
      audio_packet_queue_->flush();
    }
    seek_controller_->request_seek(target, vr::SeekType::Exact);
    playback_.seek_clock(target);
    last_tick_frame_pts_us_ = std::numeric_limits<int64_t>::min();
  }

  int64_t current_pts_us() const {
    return playback_.clock().current_pts_us();
  }

  int64_t duration_us() const { return duration_us_; }
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  bool is_playing() const { return playing_; }
  bool has_audio() const { return audio_available_; }
  int32_t audio_sample_rate() const { return audio_sample_rate_; }
  int32_t audio_channels() const { return audio_channels_; }
  int32_t active_audio_track() const {
    const auto* audio = playback_.audio_output();
    return audio ? audio->active_track() : -1;
  }

  bool copy_current_frame(VPMacOSNativeFrame* out, std::string& error) {
    if (!track_buffer_) {
      error = "player is not open";
      return false;
    }
    advance_to_clock(nullptr);
    auto frame = track_buffer_->peek(0);
    if (!frame.has_value()) {
      error = "no decoded frame is ready";
      return false;
    }
    if (!vp_macos::copy_texture_frame_to_owned_bgra(*frame, out)) {
      vp_macos::free_owned_bgra_frame(out);
      error = "decoded frame storage is not supported by the macOS BGRA bridge";
      return false;
    }
    return true;
  }

  bool copy_current_frame_into(uint8_t* dst,
                               size_t dst_size,
                               int32_t width,
                               int32_t height,
                               int32_t stride_bytes,
                               VPMacOSNativeFrameInfo* out,
                               std::string& error) {
    if (!track_buffer_) {
      error = "player is not open";
      return false;
    }
    advance_to_clock(nullptr);
    auto frame = track_buffer_->peek(0);
    if (!frame.has_value()) {
      error = "no decoded frame is ready";
      return false;
    }
    if (!vp_macos::copy_texture_frame_to_bgra_destination(
            *frame, dst, dst_size, width, height, stride_bytes, out)) {
      error = "decoded frame cannot be copied into the supplied BGRA buffer";
      return false;
    }
    return true;
  }

  bool tick_playback() {
    tick_loop_range();
    if (!playing_) {
      return false;
    }
    int64_t frame_pts_us = std::numeric_limits<int64_t>::min();
    if (!advance_to_clock(&frame_pts_us)) {
      return false;
    }
    if (frame_pts_us == last_tick_frame_pts_us_) {
      return false;
    }
    last_tick_frame_pts_us_ = frame_pts_us;
    return true;
  }

  void tick_loop_range() {
    if (!track_buffer_ || !seek_controller_) {
      return;
    }
    vr::LoopRangeSeekInput input;
    input.playing = playing_;
    input.loop_enabled = loop_range_.enabled;
    input.clock_paused = playback_.clock().is_paused();
    input.current_pts_us = playback_.clock().current_pts_us();
    input.start_us = loop_range_.start_us;
    input.end_us = loop_range_.end_us;
    const auto decision = vr::choose_loop_range_seek(input);
    if (decision.should_seek) {
      seek(decision.target_pts_us);
    }
  }

private:
  bool advance_to_clock(int64_t* frame_pts_us) {
    if (!track_buffer_) {
      return false;
    }
    const int64_t target_pts = playback_.clock().current_pts_us();
    while (true) {
      auto next = track_buffer_->peek(1);
      if (!next.has_value() || next->pts_us > target_pts) {
        break;
      }
      if (!track_buffer_->advance()) {
        break;
      }
    }
    auto frame = track_buffer_->peek(0);
    if (!frame.has_value()) {
      return false;
    }
    if (frame_pts_us) {
      *frame_pts_us = frame->pts_us;
    }
    return true;
  }

  std::string path_;
  std::unique_ptr<vr::SeekController> seek_controller_;
  std::unique_ptr<vr::PacketQueue> packet_queue_;
  std::unique_ptr<vr::PacketQueue> audio_packet_queue_;
  std::unique_ptr<vr::TrackBuffer> track_buffer_;
  std::unique_ptr<vr::DemuxThread> demux_;
  std::unique_ptr<vr::DecodeThread> decoder_;
  vr::PlaybackController playback_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int64_t duration_us_ = 0;
  bool audio_available_ = false;
  int32_t audio_sample_rate_ = 0;
  int32_t audio_channels_ = 0;
  vr::LoopRangeState loop_range_;
  int64_t last_tick_frame_pts_us_ = std::numeric_limits<int64_t>::min();
  bool playing_ = false;
};

}  // namespace

struct VPMacOSNativePlayer {
  VPMacOSNativePlayer() = default;
  ~VPMacOSNativePlayer() { stop_tick_thread(); }

  VPMacOSNativePlayer(const VPMacOSNativePlayer&) = delete;
  VPMacOSNativePlayer& operator=(const VPMacOSNativePlayer&) = delete;

  bool start_tick_thread() {
    if (tick_thread.joinable()) {
      return true;
    }
    tick_stop.store(false);
    try {
      tick_thread = std::thread([this] { run_tick_thread(); });
    } catch (...) {
      return false;
    }
    return true;
  }

  void stop_tick_thread() {
    tick_stop.store(true);
    tick_cv.notify_all();
    if (tick_thread.joinable()) {
      tick_thread.join();
    }
  }

  void run_tick_thread() {
    std::unique_lock<std::mutex> lock(tick_mutex);
    while (!tick_stop.load()) {
      tick_cv.wait_for(lock, std::chrono::milliseconds(10));
      if (tick_stop.load()) {
        break;
      }
      lock.unlock();
      bool notify_frame = false;
      {
        std::lock_guard<std::mutex> player_lock(mutex);
        notify_frame = core.tick_playback();
      }
      if (!notify_frame) {
        lock.lock();
        continue;
      }
      VPMacOSFrameAvailableCallback callback = nullptr;
      void* user_data = nullptr;
      {
        std::lock_guard<std::mutex> callback_lock(callback_mutex);
        callback = frame_available_callback;
        user_data = frame_available_user_data;
      }
      if (callback) {
        callback(user_data);
      }
      lock.lock();
    }
  }

  std::mutex mutex;
  MacOSNativePlayerCore core;
  std::mutex callback_mutex;
  VPMacOSFrameAvailableCallback frame_available_callback = nullptr;
  void* frame_available_user_data = nullptr;
  std::mutex tick_mutex;
  std::condition_variable tick_cv;
  std::atomic<bool> tick_stop{false};
  std::thread tick_thread;
};

VPMacOSNativePlayer* VPMacOSNativePlayerCreate(void) {
  auto* player = new (std::nothrow) VPMacOSNativePlayer();
  if (!player) {
    return nullptr;
  }
  if (!player->start_tick_thread()) {
    delete player;
    return nullptr;
  }
  return player;
}

void VPMacOSNativePlayerDestroy(VPMacOSNativePlayer* player) {
  delete player;
}

int VPMacOSNativePlayerOpen(VPMacOSNativePlayer* player,
                            const char* path,
                            char* error,
                            size_t error_size) {
  if (!player) {
    write_error(error, error_size, "player is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.open(path, message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

void VPMacOSNativePlayerClose(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.close();
}

void VPMacOSNativePlayerSetFrameAvailableCallback(
    VPMacOSNativePlayer* player,
    VPMacOSFrameAvailableCallback callback,
    void* user_data) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->callback_mutex);
  player->frame_available_callback = callback;
  player->frame_available_user_data = user_data;
}

void VPMacOSNativePlayerPlay(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.play();
}

void VPMacOSNativePlayerPause(VPMacOSNativePlayer* player) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.pause();
}

void VPMacOSNativePlayerSetSpeed(VPMacOSNativePlayer* player, double speed) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.set_speed(speed);
}

void VPMacOSNativePlayerSetLoopRange(VPMacOSNativePlayer* player,
                                     int enabled,
                                     int64_t start_us,
                                     int64_t end_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.set_loop_range(enabled != 0, start_us, end_us);
}

void VPMacOSNativePlayerSetAudibleTrack(VPMacOSNativePlayer* player,
                                        int32_t file_id) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.set_audible_track(file_id);
}

void VPMacOSNativePlayerSeek(VPMacOSNativePlayer* player, int64_t pts_us) {
  if (!player) {
    return;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  player->core.seek(pts_us);
}

int64_t VPMacOSNativePlayerCurrentPtsUs(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.current_pts_us();
}

int64_t VPMacOSNativePlayerDurationUs(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.duration_us();
}

int32_t VPMacOSNativePlayerWidth(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.width();
}

int32_t VPMacOSNativePlayerHeight(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.height();
}

int VPMacOSNativePlayerIsPlaying(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.is_playing() ? 1 : 0;
}

int VPMacOSNativePlayerHasAudio(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.has_audio() ? 1 : 0;
}

int32_t VPMacOSNativePlayerAudioSampleRate(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.audio_sample_rate();
}

int32_t VPMacOSNativePlayerAudioChannels(VPMacOSNativePlayer* player) {
  if (!player) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.audio_channels();
}

int32_t VPMacOSNativePlayerActiveAudioTrack(VPMacOSNativePlayer* player) {
  if (!player) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  return player->core.active_audio_track();
}

const char* VPMacOSNativePresentationAdapterName(void) {
  return vp_macos::presentation_adapter_name();
}

int VPMacOSNativePlayerCopyCurrentFrameBGRA(VPMacOSNativePlayer* player,
                                            VPMacOSNativeFrame* out,
                                            char* error,
                                            size_t error_size) {
  if (!player || !out) {
    write_error(error, error_size, "player or output frame is null");
    return -1;
  }
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.copy_current_frame(out, message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

int VPMacOSNativePlayerCopyCurrentFrameBGRAInto(VPMacOSNativePlayer* player,
                                                uint8_t* dst,
                                                size_t dst_size,
                                                int32_t width,
                                                int32_t height,
                                                int32_t stride_bytes,
                                                VPMacOSNativeFrameInfo* out,
                                                char* error,
                                                size_t error_size) {
  if (!player || !dst || !out) {
    write_error(error, error_size, "player, destination, or output info is null");
    return -1;
  }
  *out = {};
  std::lock_guard<std::mutex> lock(player->mutex);
  std::string message;
  if (!player->core.copy_current_frame_into(
          dst, dst_size, width, height, stride_bytes, out, message)) {
    write_error(error, error_size, message);
    return -1;
  }
  write_error(error, error_size, "");
  return 0;
}

void VPMacOSNativeFrameFree(VPMacOSNativeFrame* frame) {
  vp_macos::free_owned_bgra_frame(frame);
}

int VPMacOSMeasureBGRA(const uint8_t* bgra,
                       int32_t width,
                       int32_t height,
                       int32_t stride_bytes,
                       VPMacOSCaptureMetrics* out) {
  if (!out) {
    return -1;
  }
  const auto metrics = vr::measure_bgra_capture(
      bgra, width, height, stride_bytes);
  out->width = metrics.width;
  out->height = metrics.height;
  out->avg_luma = metrics.avg_luma;
  out->non_black_ratio = metrics.non_black_ratio;
  out->hash = metrics.hash;
  return bgra ? 0 : -1;
}
