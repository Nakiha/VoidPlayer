#pragma once

#include <mutex>

struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;
struct AVDictionary;

namespace vr {

using CodecOpenFunction = int (*)(AVCodecContext* ctx,
                                  const AVCodec* codec,
                                  AVDictionary** options);

enum class CodecLoopResult {
    Ok,
    Again,
    EndOfStream,
    SehCaught,
    Error,
};

int codec_loop_seh_caught_code();
CodecLoopResult classify_codec_loop_result(int ret);
bool codec_loop_is_again_or_eof(int ret);
bool codec_loop_is_seh_caught(int ret);

int open_codec_seh_guarded(AVCodecContext* ctx,
                           const AVCodec* codec,
                           AVDictionary** options,
                           CodecOpenFunction open_fn = nullptr);

int send_codec_packet_seh_guarded(AVCodecContext* ctx,
                                  const AVPacket* pkt,
                                  bool lock_device = false,
                                  std::recursive_mutex* device_mutex = nullptr);

int receive_codec_frame_seh_guarded(AVCodecContext* ctx,
                                    AVFrame* frame,
                                    bool lock_device = false,
                                    std::recursive_mutex* device_mutex = nullptr);

} // namespace vr
