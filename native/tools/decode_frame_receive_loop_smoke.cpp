#include "video_renderer/decode/codec_loop.h"
#include "video_renderer/decode/decode_frame_receive_loop.h"

#include <iostream>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace {

int fail(const char* message) {
    std::cerr << message << "\n";
    return 1;
}

} // namespace

int main() {
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return fail("failed to allocate AVFrame");
    }

    int receive_calls = 0;
    int published = 0;
    bool first_visible_marked = false;
    const auto normal_result = vr::receive_decode_frames_for_packet(
        frame,
        vr::DecodeFrameReceiveLoopOptions{false, -1, false},
        vr::DecodeFrameReceiveLoopCallbacks{
            []() { return false; },
            [&](AVFrame*) {
                ++receive_calls;
                return receive_calls <= 2 ? 0 : AVERROR(EAGAIN);
            },
            {},
            {},
            {},
            {},
            {},
            {},
            [&]() { first_visible_marked = true; },
            [&](AVFrame*) {
                ++published;
                return true;
            },
            {},
            {},
        });
    if (normal_result.frames_produced != 2 ||
        normal_result.stop_with_error ||
        receive_calls != 3 ||
        published != 2 ||
        !first_visible_marked) {
        av_frame_free(&frame);
        return fail("decode frame receive loop did not publish normal frames");
    }

    receive_calls = 0;
    int dropped = 0;
    int collected = 0;
    int previews_published = 0;
    const auto exact_seek_result = vr::receive_decode_frames_for_packet(
        frame,
        vr::DecodeFrameReceiveLoopOptions{true, 1'000'000, false},
        vr::DecodeFrameReceiveLoopCallbacks{
            []() { return false; },
            [&](AVFrame* received_frame) {
                ++receive_calls;
                if (receive_calls == 1) {
                    received_frame->pts = 700'000;
                    return 0;
                }
                if (receive_calls == 2) {
                    received_frame->pts = 800'000;
                    return 0;
                }
                return AVERROR(EAGAIN);
            },
            {},
            {},
            [&]() { ++dropped; },
            [&](AVFrame*) { ++collected; },
            []() { return true; },
            [&]() { ++previews_published; },
            {},
            {},
            {},
            {},
        });
    if (exact_seek_result.frames_produced != 1 ||
        exact_seek_result.stop_with_error ||
        dropped != 1 ||
        collected != 1 ||
        previews_published != 1) {
        av_frame_free(&frame);
        return fail("decode frame receive loop did not handle exact seek window");
    }

    const auto fatal_result = vr::receive_decode_frames_for_packet(
        frame,
        vr::DecodeFrameReceiveLoopOptions{},
        vr::DecodeFrameReceiveLoopCallbacks{
            []() { return false; },
            [](AVFrame*) { return vr::codec_loop_seh_caught_code(); },
            {},
            {},
            {},
            {},
            {},
            {},
            {},
            {},
            {},
            {},
        });
    if (fatal_result.frames_produced != 0 || !fatal_result.stop_with_error) {
        av_frame_free(&frame);
        return fail("decode frame receive loop did not report fatal receive");
    }

    av_frame_free(&frame);
    return 0;
}
