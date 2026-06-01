#include "video_renderer/decode/codec_loop.h"
#include "video_renderer/decode/decode_frame_drainer.h"

#include <iostream>

extern "C" {
#include <libavutil/error.h>
}

namespace {

int fail(const char* message) {
    std::cerr << message << "\n";
    return 1;
}

} // namespace

int main() {
    int receive_calls = 0;
    int publish_calls = 0;
    const auto publish_until_eagain = vr::drain_frames_before_next_packet(
        nullptr,
        vr::DecodeFrameDrainCallbacks{
            []() { return false; },
            [&](AVFrame*) {
                ++receive_calls;
                return receive_calls <= 2 ? 0 : AVERROR(EAGAIN);
            },
            {},
            {},
            [&](AVFrame*) {
                ++publish_calls;
                return true;
            },
            []() { return false; },
        });
    if (publish_until_eagain.frames_published != 2 ||
        !publish_until_eagain.clear_drain_request ||
        publish_until_eagain.stop_with_error ||
        receive_calls != 3 ||
        publish_calls != 2) {
        return fail("decode frame drainer did not publish until EAGAIN");
    }

    int aborted_receive_calls = 0;
    const auto abort_before_receive = vr::drain_frames_before_next_packet(
        nullptr,
        vr::DecodeFrameDrainCallbacks{
            []() { return true; },
            [&](AVFrame*) {
                ++aborted_receive_calls;
                return 0;
            },
            {},
            {},
            {},
            {},
        });
    if (abort_before_receive.frames_published != 0 ||
        !abort_before_receive.clear_drain_request ||
        abort_before_receive.stop_with_error ||
        aborted_receive_calls != 0) {
        return fail("decode frame drainer did not abort before receive");
    }

    const auto fatal_receive = vr::drain_frames_before_next_packet(
        nullptr,
        vr::DecodeFrameDrainCallbacks{
            []() { return false; },
            [](AVFrame*) { return vr::codec_loop_seh_caught_code(); },
            {},
            {},
            {},
            {},
        });
    if (fatal_receive.frames_published != 0 ||
        !fatal_receive.clear_drain_request ||
        !fatal_receive.stop_with_error) {
        return fail("decode frame drainer did not report fatal receive");
    }

    const auto publish_failure = vr::drain_frames_before_next_packet(
        nullptr,
        vr::DecodeFrameDrainCallbacks{
            []() { return false; },
            [](AVFrame*) { return 0; },
            {},
            {},
            [](AVFrame*) { return false; },
            []() { return false; },
        });
    if (publish_failure.frames_published != 0 ||
        publish_failure.clear_drain_request ||
        publish_failure.stop_with_error) {
        return fail("decode frame drainer changed drain request after publish failure");
    }

    return 0;
}
