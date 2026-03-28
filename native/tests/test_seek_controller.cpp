#include <catch2/catch_test_macros.hpp>
#include "video_renderer/sync/seek_controller.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <string>
#include <cmath>

using namespace vr;

TEST_CASE("SeekController: pending request stores target", "[seek_controller]") {
    SeekController ctrl;
    REQUIRE_FALSE(ctrl.has_pending_seek());

    ctrl.request_seek(3000000, SeekType::Keyframe);
    REQUIRE(ctrl.has_pending_seek());

    SeekRequest req = ctrl.pending_request();
    REQUIRE(req.target_pts_us == 3000000);
    REQUIRE(req.type == SeekType::Keyframe);
}

TEST_CASE("SeekController: clear_pending removes pending request", "[seek_controller]") {
    SeekController ctrl;
    ctrl.request_seek(5000000, SeekType::Exact);
    REQUIRE(ctrl.has_pending_seek());

    ctrl.clear_pending();
    REQUIRE_FALSE(ctrl.has_pending_seek());
}

TEST_CASE("SeekController: keyframe seek on H264 file", "[seek_controller]") {
    std::string path = std::string(VIDEO_TEST_DIR) + "/h264_9s_1920x1080.mp4";

    // Open the file
    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    REQUIRE(ret >= 0);
    REQUIRE(fmt_ctx != nullptr);

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    REQUIRE(ret >= 0);

    // Find video stream
    int stream_index = -1;
    AVRational time_base{1, 1};
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream_index = static_cast<int>(i);
            time_base = fmt_ctx->streams[i]->time_base;
            break;
        }
    }
    REQUIRE(stream_index >= 0);

    SeekController ctrl;
    ctrl.request_seek(5000000, SeekType::Keyframe); // Seek to 5 seconds

    bool ok = ctrl.execute_seek(fmt_ctx, stream_index, time_base);
    REQUIRE(ok);
    REQUIRE_FALSE(ctrl.has_pending_seek()); // cleared after execution

    // Read a frame to verify position is near 5s
    AVPacket* pkt = av_packet_alloc();
    ret = av_read_frame(fmt_ctx, pkt);
    if (ret >= 0 && pkt->stream_index == stream_index) {
        int64_t pts_us = av_rescale_q(pkt->pts, time_base, {1, 1000000});
        // Keyframe seek should be within 2 seconds of target
        REQUIRE(std::abs(pts_us - 5000000) < 2000000);
    }
    av_packet_free(&pkt);

    avformat_close_input(&fmt_ctx);
}

TEST_CASE("SeekController: keyframe seek on HEVC file", "[seek_controller]") {
    std::string path = std::string(VIDEO_TEST_DIR) + "/h265_10s_1920x1080.mp4";

    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    REQUIRE(ret >= 0);

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    REQUIRE(ret >= 0);

    int stream_index = -1;
    AVRational time_base{1, 1};
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream_index = static_cast<int>(i);
            time_base = fmt_ctx->streams[i]->time_base;
            break;
        }
    }
    REQUIRE(stream_index >= 0);

    SeekController ctrl;
    ctrl.request_seek(5000000, SeekType::Keyframe);

    bool ok = ctrl.execute_seek(fmt_ctx, stream_index, time_base);
    REQUIRE(ok);

    // Read a frame to verify position
    AVPacket* pkt = av_packet_alloc();
    ret = av_read_frame(fmt_ctx, pkt);
    if (ret >= 0 && pkt->stream_index == stream_index) {
        int64_t pts_us = av_rescale_q(pkt->pts, time_base, {1, 1000000});
        REQUIRE(std::abs(pts_us - 5000000) < 2000000);
    }
    av_packet_free(&pkt);

    avformat_close_input(&fmt_ctx);
}

TEST_CASE("SeekController: multiple requests overwrite previous", "[seek_controller]") {
    SeekController ctrl;

    ctrl.request_seek(1000000, SeekType::Keyframe);
    ctrl.request_seek(8000000, SeekType::Exact);

    SeekRequest req = ctrl.pending_request();
    REQUIRE(req.target_pts_us == 8000000);
    REQUIRE(req.type == SeekType::Exact);
}
