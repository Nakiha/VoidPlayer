#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/decode/decode_thread.h"
#include "video_renderer/decode/demux_thread.h"
#include "video_renderer/buffer/packet_queue.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/sync/seek_controller.h"
#include <thread>
#include <chrono>
#include <string>

using namespace vr;

TEST_CASE("DecodeThread: software decode H264 produces monotonically increasing PTS", "[decode_thread]") {
    std::string path = vr::test::video_test_dir() + "/h264_9s_1920x1080.mp4";

    PacketQueue pkt_queue(100);
    TrackBuffer track_buffer(8, 2);

    // Create and start demux thread
    SeekController sc;
    DemuxThread demux(path, pkt_queue, sc);
    REQUIRE(demux.start());

    // Wait briefly for demux to parse stream info
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const DemuxStats& stats = demux.stats();
    REQUIRE(stats.video_stream_index >= 0);
    REQUIRE(stats.codec_params != nullptr);

    // Create and start decode thread
    DecodeThread decoder(pkt_queue, track_buffer, stats.codec_params, stats.time_base);
    REQUIRE(decoder.start());

    // Wait for some frames to be decoded
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t frame_count = 0;
    while (std::chrono::steady_clock::now() < deadline && frame_count < 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        frame_count = track_buffer.total_count();
    }

    REQUIRE(frame_count >= 2);

    // Verify monotonically increasing PTS
    int64_t prev_pts = INT64_MIN;
    for (size_t i = 0; i < frame_count; ++i) {
        auto frame = track_buffer.peek(static_cast<int>(i));
        REQUIRE(frame.has_value());
        REQUIRE(frame->pts_us >= prev_pts);
        prev_pts = frame->pts_us;
    }

    // Stop threads
    decoder.stop();
    demux.stop();
}

TEST_CASE("DecodeThread: software decode HEVC produces frames", "[decode_thread]") {
    std::string path = vr::test::video_test_dir() + "/h265_10s_1920x1080.mp4";

    PacketQueue pkt_queue(100);
    TrackBuffer track_buffer(8, 2);

    SeekController sc;
    DemuxThread demux(path, pkt_queue, sc);
    REQUIRE(demux.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const DemuxStats& stats = demux.stats();
    REQUIRE(stats.video_stream_index >= 0);

    DecodeThread decoder(pkt_queue, track_buffer, stats.codec_params, stats.time_base);
    REQUIRE(decoder.start());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t frame_count = 0;
    while (std::chrono::steady_clock::now() < deadline && frame_count < 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        frame_count = track_buffer.total_count();
    }

    REQUIRE(frame_count >= 2);

    decoder.stop();
    demux.stop();
}

TEST_CASE("DecodeThread: decoded frames have non-zero duration", "[decode_thread]") {
    std::string path = vr::test::video_test_dir() + "/h264_9s_1920x1080.mp4";

    PacketQueue pkt_queue(100);
    TrackBuffer track_buffer(8, 2);

    SeekController sc;
    DemuxThread demux(path, pkt_queue, sc);
    REQUIRE(demux.start());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const DemuxStats& stats = demux.stats();
    REQUIRE(stats.video_stream_index >= 0);

    DecodeThread decoder(pkt_queue, track_buffer, stats.codec_params, stats.time_base);
    REQUIRE(decoder.start());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && track_buffer.total_count() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    REQUIRE(track_buffer.total_count() >= 1);

    // Check that at least the first few frames have non-zero duration
    size_t to_check = std::min(track_buffer.total_count(), size_t(5));
    for (size_t i = 0; i < to_check; ++i) {
        auto frame = track_buffer.peek(static_cast<int>(i));
        if (frame.has_value()) {
            REQUIRE(frame->duration_us > 0);
        }
    }

    decoder.stop();
    demux.stop();
}
