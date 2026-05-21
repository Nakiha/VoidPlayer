#include "preview_frame_decoder.h"

#include "../video_renderer/decode/frame_timestamp_rescaler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace {

void write_error(char* error, size_t error_size, const char* message) {
  if (!error || error_size == 0) {
    return;
  }
  std::snprintf(error, error_size, "%s", message ? message : "unknown error");
}

void write_ffmpeg_error(char* error, size_t error_size, const char* prefix, int ret) {
  char details[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(ret, details, sizeof(details));
  char message[512] = {};
  std::snprintf(message, sizeof(message), "%s: %s", prefix, details);
  write_error(error, error_size, message);
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

bool convert_yuv420p_to_bgra(const AVFrame* frame, uint8_t* dst) {
  const bool full_range = frame->color_range == AVCOL_RANGE_JPEG ||
                          frame->format == AV_PIX_FMT_YUVJ420P;
  const int width = frame->width;
  const int height = frame->height;
  const uint8_t* y_plane = frame->data[0];
  const uint8_t* u_plane = frame->data[1];
  const uint8_t* v_plane = frame->data[2];
  if (!y_plane || !u_plane || !v_plane) {
    return false;
  }

  for (int y = 0; y < height; ++y) {
    const uint8_t* y_row = y_plane + y * frame->linesize[0];
    const uint8_t* u_row = u_plane + (y / 2) * frame->linesize[1];
    const uint8_t* v_row = v_plane + (y / 2) * frame->linesize[2];
    uint8_t* dst_row = dst + static_cast<size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x) {
      yuv_to_bgra(y_row[x], u_row[x / 2], v_row[x / 2], full_range, dst_row + x * 4);
    }
  }
  return true;
}

bool convert_nv12_to_bgra(const AVFrame* frame, uint8_t* dst) {
  const bool full_range = frame->color_range == AVCOL_RANGE_JPEG;
  const int width = frame->width;
  const int height = frame->height;
  const uint8_t* y_plane = frame->data[0];
  const uint8_t* uv_plane = frame->data[1];
  if (!y_plane || !uv_plane) {
    return false;
  }

  for (int y = 0; y < height; ++y) {
    const uint8_t* y_row = y_plane + y * frame->linesize[0];
    const uint8_t* uv_row = uv_plane + (y / 2) * frame->linesize[1];
    uint8_t* dst_row = dst + static_cast<size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x) {
      const int uv_index = (x / 2) * 2;
      yuv_to_bgra(y_row[x], uv_row[uv_index], uv_row[uv_index + 1], full_range, dst_row + x * 4);
    }
  }
  return true;
}

bool convert_frame_to_bgra(const AVFrame* frame, uint8_t* dst) {
  switch (static_cast<AVPixelFormat>(frame->format)) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
      return convert_yuv420p_to_bgra(frame, dst);
    case AV_PIX_FMT_NV12:
      return convert_nv12_to_bgra(frame, dst);
    default:
      return false;
  }
}

int64_t duration_us(const AVFormatContext* format_ctx, const AVStream* stream) {
  if (format_ctx->duration > 0) {
    return format_ctx->duration;
  }
  if (stream->duration > 0) {
    return av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000000});
  }
  return 0;
}

int64_t frame_pts_us(const AVFrame* frame) {
  if (frame->pts == AV_NOPTS_VALUE) {
    return 0;
  }
  return frame->pts;
}

}  // namespace

int VPMacOSDecodeVideoFrameBGRA(const char* path,
                                int64_t target_pts_us,
                                VPMacOSDecodedFrame* out,
                                char* error,
                                size_t error_size) {
  if (!path || !out) {
    write_error(error, error_size, "invalid decoder arguments");
    return -1;
  }

  std::memset(out, 0, sizeof(*out));

  AVFormatContext* format_ctx = nullptr;
  int ret = avformat_open_input(&format_ctx, path, nullptr, nullptr);
  if (ret < 0) {
    write_ffmpeg_error(error, error_size, "avformat_open_input failed", ret);
    return ret;
  }

  auto close_format = [&]() {
    if (format_ctx) {
      avformat_close_input(&format_ctx);
    }
  };

  ret = avformat_find_stream_info(format_ctx, nullptr);
  if (ret < 0) {
    write_ffmpeg_error(error, error_size, "avformat_find_stream_info failed", ret);
    close_format();
    return ret;
  }

  const int stream_index =
      av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (stream_index < 0) {
    write_ffmpeg_error(error, error_size, "failed to find video stream", stream_index);
    close_format();
    return stream_index;
  }

  AVStream* stream = format_ctx->streams[stream_index];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) {
    write_error(error, error_size, "failed to find video decoder");
    close_format();
    return AVERROR_DECODER_NOT_FOUND;
  }

  AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) {
    write_error(error, error_size, "failed to allocate codec context");
    close_format();
    return AVERROR(ENOMEM);
  }

  ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
  if (ret < 0) {
    write_ffmpeg_error(error, error_size, "avcodec_parameters_to_context failed", ret);
    avcodec_free_context(&codec_ctx);
    close_format();
    return ret;
  }

  ret = avcodec_open2(codec_ctx, codec, nullptr);
  if (ret < 0) {
    write_ffmpeg_error(error, error_size, "avcodec_open2 failed", ret);
    avcodec_free_context(&codec_ctx);
    close_format();
    return ret;
  }

  const int64_t clamped_target_pts_us = std::max<int64_t>(0, target_pts_us);
  if (clamped_target_pts_us > 0) {
    const int64_t target_ts =
        av_rescale_q(clamped_target_pts_us, AVRational{1, 1000000}, stream->time_base);
    ret = av_seek_frame(format_ctx, stream_index, target_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
      write_ffmpeg_error(error, error_size, "av_seek_frame failed", ret);
      avcodec_free_context(&codec_ctx);
      close_format();
      return ret;
    }
    avcodec_flush_buffers(codec_ctx);
  }

  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  AVFrame* candidate = av_frame_alloc();
  if (!packet || !frame || !candidate) {
    write_error(error, error_size, "failed to allocate packet/frame");
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&candidate);
    avcodec_free_context(&codec_ctx);
    close_format();
    return AVERROR(ENOMEM);
  }

  bool have_candidate = false;
  auto select_candidate = [&]() -> int {
    av_frame_unref(candidate);
    const int ref_ret = av_frame_ref(candidate, frame);
    if (ref_ret < 0) {
      write_ffmpeg_error(error, error_size, "av_frame_ref failed", ref_ret);
      return ref_ret;
    }
    have_candidate = true;
    if (clamped_target_pts_us <= 0 || frame->pts == AV_NOPTS_VALUE ||
        frame_pts_us(frame) >= clamped_target_pts_us) {
      return 1;
    }
    return 0;
  };

  bool decoded = false;
  while (!decoded && (ret = av_read_frame(format_ctx, packet)) >= 0) {
    if (packet->stream_index != stream_index) {
      av_packet_unref(packet);
      continue;
    }

    ret = avcodec_send_packet(codec_ctx, packet);
    av_packet_unref(packet);
    if (ret < 0) {
      write_ffmpeg_error(error, error_size, "avcodec_send_packet failed", ret);
      break;
    }

    while ((ret = avcodec_receive_frame(codec_ctx, frame)) == 0) {
      vr::rescale_frame_timestamps_to_us(frame, stream->time_base);
      const int selection = select_candidate();
      av_frame_unref(frame);
      if (selection < 0) {
        ret = selection;
        break;
      }
      if (selection > 0) {
        decoded = true;
        break;
      }
    }
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      break;
    }
    if (ret != 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
      write_ffmpeg_error(error, error_size, "avcodec_receive_frame failed", ret);
      break;
    }
  }

  if (!decoded && ret == AVERROR_EOF) {
    ret = avcodec_send_packet(codec_ctx, nullptr);
    if (ret >= 0 || ret == AVERROR_EOF) {
      while ((ret = avcodec_receive_frame(codec_ctx, frame)) == 0) {
        vr::rescale_frame_timestamps_to_us(frame, stream->time_base);
        const int selection = select_candidate();
        av_frame_unref(frame);
        if (selection < 0) {
          ret = selection;
          break;
        }
        if (selection > 0) {
          decoded = true;
          break;
        }
      }
    }
  }
  if (!decoded && have_candidate) {
    decoded = true;
  }

  if (!decoded) {
    if (ret >= 0 || ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
      write_error(error, error_size, "no decodable video frame");
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&candidate);
    avcodec_free_context(&codec_ctx);
    close_format();
    return ret < 0 ? ret : AVERROR(EINVAL);
  }

  const size_t bgra_size = static_cast<size_t>(candidate->width) * candidate->height * 4;
  uint8_t* bgra = static_cast<uint8_t*>(std::malloc(bgra_size));
  if (!bgra) {
    write_error(error, error_size, "failed to allocate BGRA frame");
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&candidate);
    avcodec_free_context(&codec_ctx);
    close_format();
    return AVERROR(ENOMEM);
  }

  if (!convert_frame_to_bgra(candidate, bgra)) {
    write_error(error, error_size, "unsupported decoded pixel format");
    std::free(bgra);
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&candidate);
    avcodec_free_context(&codec_ctx);
    close_format();
    return AVERROR(EINVAL);
  }

  out->width = candidate->width;
  out->height = candidate->height;
  out->duration_us = duration_us(format_ctx, stream);
  out->pts_us = frame_pts_us(candidate);
  out->bgra = bgra;
  out->bgra_size = bgra_size;

  av_packet_free(&packet);
  av_frame_free(&frame);
  av_frame_free(&candidate);
  avcodec_free_context(&codec_ctx);
  close_format();
  return 0;
}

int VPMacOSDecodeFirstVideoFrameBGRA(const char* path,
                                     VPMacOSDecodedFrame* out,
                                     char* error,
                                     size_t error_size) {
  return VPMacOSDecodeVideoFrameBGRA(path, 0, out, error, error_size);
}

void VPMacOSDecodedFrameFree(VPMacOSDecodedFrame* frame) {
  if (!frame) {
    return;
  }
  std::free(frame->bgra);
  std::memset(frame, 0, sizeof(*frame));
}
