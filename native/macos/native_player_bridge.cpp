#include "native_player_bridge.h"

#include "media/demux_thread.h"
#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/clock.h"
#include "video_renderer/decode/decode_thread.h"
#include "video_renderer/frame/frame_storage.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace {

void write_error(char* error, size_t error_size, const std::string& message) {
  if (!error || error_size == 0) {
    return;
  }
  const size_t copy_size = std::min(error_size - 1, message.size());
  std::memcpy(error, message.data(), copy_size);
  error[copy_size] = '\0';
}

uint8_t clamp_u8(int value) {
  return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

void yuv_to_bgra(uint8_t y,
                 uint8_t u,
                 uint8_t v,
                 bool full_range,
                 uint8_t* out) {
  const int uu = static_cast<int>(u) - 128;
  const int vv = static_cast<int>(v) - 128;
  int r = 0;
  int g = 0;
  int b = 0;
  if (full_range) {
    const int yy = static_cast<int>(y);
    r = (256 * yy + 359 * vv + 128) >> 8;
    g = (256 * yy - 88 * uu - 183 * vv + 128) >> 8;
    b = (256 * yy + 454 * uu + 128) >> 8;
  } else {
    const int yy = std::max(0, static_cast<int>(y) - 16);
    r = (298 * yy + 409 * vv + 128) >> 8;
    g = (298 * yy - 100 * uu - 208 * vv + 128) >> 8;
    b = (298 * yy + 516 * uu + 128) >> 8;
  }
  out[0] = clamp_u8(b);
  out[1] = clamp_u8(g);
  out[2] = clamp_u8(r);
  out[3] = 255;
}

bool allocate_bgra(const vr::TextureFrame& frame, VPMacOSNativeFrame* out) {
  if (!out || frame.width <= 0 || frame.height <= 0) {
    return false;
  }
  const size_t size = static_cast<size_t>(frame.width) *
      static_cast<size_t>(frame.height) * 4u;
  auto* bgra = new (std::nothrow) uint8_t[size];
  if (!bgra) {
    return false;
  }
  out->width = frame.width;
  out->height = frame.height;
  out->pts_us = frame.pts_us;
  out->dts_us = frame.dts_us;
  out->duration_us = frame.duration_us;
  out->bgra = bgra;
  out->bgra_size = size;
  return true;
}

bool copy_cpu_rgba(const vr::TextureFrame& frame, VPMacOSNativeFrame* out) {
  const auto* storage = frame.cpu_rgba_storage();
  if (!storage || !storage->data || storage->stride < frame.width * 4 ||
      !allocate_bgra(frame, out)) {
    return false;
  }
  for (int y = 0; y < frame.height; ++y) {
    const size_t src_offset = static_cast<size_t>(y) * storage->stride;
    const size_t dst_offset = static_cast<size_t>(y) * frame.width * 4u;
    if (src_offset + static_cast<size_t>(frame.width) * 4u > storage->data->size()) {
      VPMacOSNativeFrameFree(out);
      return false;
    }
    std::memcpy(out->bgra + dst_offset,
                storage->data->data() + src_offset,
                static_cast<size_t>(frame.width) * 4u);
  }
  return true;
}

bool copy_cpu_planar_yuv420(const vr::TextureFrame& frame, VPMacOSNativeFrame* out) {
  const auto* storage = frame.cpu_planar_yuv_storage();
  if (!storage || storage->bytes_per_sample != 1 || !storage->planes[0] ||
      !storage->planes[1] || !storage->planes[2] || !allocate_bgra(frame, out)) {
    return false;
  }
  const bool full_range = frame.color.range == vr::VIDEO_COLOR_RANGE_FULL;
  for (int y = 0; y < frame.height; ++y) {
    const uint8_t* y_row = storage->planes[0] +
        static_cast<size_t>(y) * storage->strides[0];
    const uint8_t* u_row = storage->planes[1] +
        static_cast<size_t>(y / 2) * storage->strides[1];
    const uint8_t* v_row = storage->planes[2] +
        static_cast<size_t>(y / 2) * storage->strides[2];
    uint8_t* dst_row = out->bgra + static_cast<size_t>(y) * frame.width * 4u;
    for (int x = 0; x < frame.width; ++x) {
      yuv_to_bgra(y_row[x], u_row[x / 2], v_row[x / 2],
                  full_range, dst_row + x * 4);
    }
  }
  return true;
}

bool copy_cpu_nv12(const vr::TextureFrame& frame, VPMacOSNativeFrame* out) {
  const auto* storage = frame.cpu_nv12_storage();
  if (!storage || storage->is_p010 || !storage->data || storage->y_stride <= 0 ||
      storage->uv_stride <= 0 || !allocate_bgra(frame, out)) {
    return false;
  }
  const bool full_range = frame.color.range == vr::VIDEO_COLOR_RANGE_FULL;
  const uint8_t* y_plane = storage->data->data();
  const uint8_t* uv_plane = y_plane +
      static_cast<size_t>(storage->y_stride) * storage->coded_height;
  for (int y = 0; y < frame.height; ++y) {
    const uint8_t* y_row = y_plane + static_cast<size_t>(y) * storage->y_stride;
    const uint8_t* uv_row = uv_plane + static_cast<size_t>(y / 2) * storage->uv_stride;
    uint8_t* dst_row = out->bgra + static_cast<size_t>(y) * frame.width * 4u;
    for (int x = 0; x < frame.width; ++x) {
      const int uv_index = (x / 2) * 2;
      yuv_to_bgra(y_row[x], uv_row[uv_index], uv_row[uv_index + 1],
                  full_range, dst_row + x * 4);
    }
  }
  return true;
}

bool copy_frame_to_bgra(const vr::TextureFrame& frame, VPMacOSNativeFrame* out) {
  if (!out) {
    return false;
  }
  *out = {};
  switch (frame.storage_kind()) {
  case vr::FrameStorageKind::CpuRgba:
    return copy_cpu_rgba(frame, out);
  case vr::FrameStorageKind::CpuPlanarYuv:
    return copy_cpu_planar_yuv420(frame, out);
  case vr::FrameStorageKind::CpuNv12:
    return copy_cpu_nv12(frame, out);
  default:
    return false;
  }
}

class MacOSNativePlayerCore {
public:
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
    track_buffer_ = std::make_unique<vr::TrackBuffer>(16, 4);
    demux_ = std::make_unique<vr::DemuxThread>(path_, *packet_queue_, *seek_controller_);

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

    clock_.seek(0);
    clock_.pause();
    playing_ = false;
    return true;
  }

  void close() {
    playing_ = false;
    if (decoder_) {
      decoder_->stop();
    }
    if (demux_) {
      demux_->stop();
    }
    decoder_.reset();
    demux_.reset();
    track_buffer_.reset();
    packet_queue_.reset();
    seek_controller_.reset();
    path_.clear();
    width_ = 0;
    height_ = 0;
    duration_us_ = 0;
  }

  void play() {
    if (!decoder_) {
      return;
    }
    playing_ = true;
    clock_.resume();
  }

  void pause() {
    playing_ = false;
    clock_.pause();
  }

  void set_speed(double speed) {
    clock_.set_speed(std::max(0.01, speed));
  }

  void seek(int64_t pts_us) {
    if (!seek_controller_ || !track_buffer_) {
      return;
    }
    const int64_t target = std::max<int64_t>(0, pts_us);
    track_buffer_->set_state(vr::TrackState::Flushing);
    track_buffer_->clear_frames();
    packet_queue_->flush();
    seek_controller_->request_seek(target, vr::SeekType::Exact);
    clock_.seek(target);
  }

  int64_t current_pts_us() const {
    return clock_.current_pts_us();
  }

  int64_t duration_us() const { return duration_us_; }
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }
  bool is_playing() const { return playing_; }

  bool copy_current_frame(VPMacOSNativeFrame* out, std::string& error) {
    if (!track_buffer_) {
      error = "player is not open";
      return false;
    }
    const int64_t target_pts = clock_.current_pts_us();
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
      error = "no decoded frame is ready";
      return false;
    }
    if (!copy_frame_to_bgra(*frame, out)) {
      VPMacOSNativeFrameFree(out);
      error = "decoded frame storage is not supported by the macOS BGRA bridge";
      return false;
    }
    return true;
  }

private:
  std::string path_;
  std::unique_ptr<vr::SeekController> seek_controller_;
  std::unique_ptr<vr::PacketQueue> packet_queue_;
  std::unique_ptr<vr::TrackBuffer> track_buffer_;
  std::unique_ptr<vr::DemuxThread> demux_;
  std::unique_ptr<vr::DecodeThread> decoder_;
  vr::Clock clock_;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int64_t duration_us_ = 0;
  bool playing_ = false;
};

}  // namespace

struct VPMacOSNativePlayer {
  std::mutex mutex;
  MacOSNativePlayerCore core;
};

VPMacOSNativePlayer* VPMacOSNativePlayerCreate(void) {
  return new (std::nothrow) VPMacOSNativePlayer();
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

void VPMacOSNativeFrameFree(VPMacOSNativeFrame* frame) {
  if (!frame) {
    return;
  }
  delete[] frame->bgra;
  *frame = {};
}
