#include <catch2/catch_test_macros.hpp>

#include "media/av_packet_lifetime.h"
#include "media/media_input_session.h"
#include "test_utils.h"

#include <string>

using namespace vr;

TEST_CASE("MediaInputSession owns probe read seek and close") {
    const std::string path =
        vr::test::video_test_dir() +
        "/h264_9s_1920x1080.mp4";

    MediaInputSession input;
    MediaInputOpenOptions options;
    std::string error;
    REQUIRE(input.open(path, options, error));
    REQUIRE(error.empty());
    REQUIRE(input.is_open());
    REQUIRE_FALSE(input.uses_private_demuxer());
    REQUIRE(std::string(input.backend_name()) == "libavformat");
    REQUIRE(input.format_context() != nullptr);

    const int video_stream =
        input.best_stream_index(AVMEDIA_TYPE_VIDEO);
    REQUIRE(video_stream >= 0);
    REQUIRE(input.codec_parameters(video_stream) != nullptr);
    REQUIRE(
        input.codec_parameters(video_stream)->codec_id ==
        AV_CODEC_ID_H264);
    const AVRational time_base =
        input.time_base_for_stream(video_stream);
    REQUIRE(time_base.num > 0);
    REQUIRE(time_base.den > 0);
    REQUIRE(input.stats().width == 1920);
    REQUIRE(input.stats().height == 1080);

    auto packet = AvPacketOwner::allocate();
    REQUIRE(packet);
    bool found_video_packet = false;
    for (int i = 0; i < 64; ++i) {
        const int result = input.read_packet(packet.get());
        REQUIRE(result >= 0);
        if (packet.get()->stream_index == video_stream) {
            found_video_packet = true;
            break;
        }
        av_packet_unref(packet.get());
    }
    REQUIRE(found_video_packet);
    av_packet_unref(packet.get());

    REQUIRE(input.seek(video_stream, 0) >= 0);
    input.flush();
    input.close();
    REQUIRE_FALSE(input.is_open());
    REQUIRE(input.format_context() == nullptr);
}

TEST_CASE("MediaInputSession reports open failure and stays reusable") {
    MediaInputSession input;
    MediaInputOpenOptions options;
    std::string error;

    REQUIRE_FALSE(input.open(
        "__voidplayer_missing_input__.mp4",
        options,
        error));
    REQUIRE_FALSE(error.empty());
    REQUIRE_FALSE(input.is_open());

    const std::string path =
        vr::test::video_test_dir() +
        "/h264_9s_1920x1080.mp4";
    REQUIRE(input.open(path, options, error));
    REQUIRE(input.is_open());
}
