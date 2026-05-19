#include "video_renderer/decode/codec_loop.h"

#include <spdlog/spdlog.h>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

namespace vr {
namespace {

constexpr int kCodecLoopSehCaught = AVERROR_EXTERNAL;

#ifdef _WIN32

// D3D11 internals can throw cross-module SEH exceptions through FFmpeg codec
// calls. Keep each __try/__except in a noinline function without C++ objects
// with destructors so MSVC does not merge the SEH scope into a caller.
__declspec(noinline)
int seh_send_packet(AVCodecContext* ctx, const AVPacket* pkt) {
    __try {
        return avcodec_send_packet(ctx, pkt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        spdlog::error("[DecodeThread] SEH exception in avcodec_send_packet: {:#x}",
                      static_cast<unsigned long>(code));
        return kCodecLoopSehCaught;
    }
}

__declspec(noinline)
int seh_receive_frame(AVCodecContext* ctx, AVFrame* frame) {
    __try {
        return avcodec_receive_frame(ctx, frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        spdlog::error("[DecodeThread] SEH exception in avcodec_receive_frame: {:#x}",
                      static_cast<unsigned long>(code));
        return kCodecLoopSehCaught;
    }
}

#else

int seh_send_packet(AVCodecContext* ctx, const AVPacket* pkt) {
    return avcodec_send_packet(ctx, pkt);
}

int seh_receive_frame(AVCodecContext* ctx, AVFrame* frame) {
    return avcodec_receive_frame(ctx, frame);
}

#endif

} // namespace

int codec_loop_seh_caught_code() {
    return kCodecLoopSehCaught;
}

CodecLoopResult classify_codec_loop_result(int ret) {
    if (ret >= 0) {
        return CodecLoopResult::Ok;
    }
    if (ret == AVERROR(EAGAIN)) {
        return CodecLoopResult::Again;
    }
    if (ret == AVERROR_EOF) {
        return CodecLoopResult::EndOfStream;
    }
    if (ret == kCodecLoopSehCaught) {
        return CodecLoopResult::SehCaught;
    }
    return CodecLoopResult::Error;
}

bool codec_loop_is_again_or_eof(int ret) {
    const auto result = classify_codec_loop_result(ret);
    return result == CodecLoopResult::Again ||
           result == CodecLoopResult::EndOfStream;
}

bool codec_loop_is_seh_caught(int ret) {
    return classify_codec_loop_result(ret) == CodecLoopResult::SehCaught;
}

int send_codec_packet_seh_guarded(AVCodecContext* ctx,
                                  const AVPacket* pkt,
                                  bool lock_device,
                                  std::recursive_mutex* device_mutex) {
    if (lock_device && device_mutex) {
        std::lock_guard<std::recursive_mutex> d3d_lock(*device_mutex);
        return seh_send_packet(ctx, pkt);
    }
    return seh_send_packet(ctx, pkt);
}

int receive_codec_frame_seh_guarded(AVCodecContext* ctx,
                                    AVFrame* frame,
                                    bool lock_device,
                                    std::recursive_mutex* device_mutex) {
    if (lock_device && device_mutex) {
        std::lock_guard<std::recursive_mutex> d3d_lock(*device_mutex);
        return seh_receive_frame(ctx, frame);
    }
    return seh_receive_frame(ctx, frame);
}

} // namespace vr
