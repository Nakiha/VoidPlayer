#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/decode/decode_thread.h"
#include "media/demux_thread.h"
#include "media/packet_queue.h"
#include "video_renderer/buffer/track_buffer.h"
#include "media/seek_controller.h"
#include <thread>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace vr;

namespace {

std::string quote_arg(const std::string& value) {
    std::string quoted = "\"";
    for (char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted += ch;
        }
    }
    quoted += "\"";
    return quoted;
}

int run_command(const std::string& command) {
#ifdef _WIN32
    return std::system(("\"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

void append_file(const std::filesystem::path& src, std::ofstream& out) {
    std::ifstream in(src, std::ios::binary);
    REQUIRE(in.good());
    out << in.rdbuf();
    REQUIRE(out.good());
}

std::filesystem::path make_dynamic_resolution_h264_fixture() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir =
        fs::temp_directory_path() / ("void_player_dynamic_res_" + std::to_string(stamp));
    fs::create_directories(dir);

    const fs::path seg_a = dir / "seg_a.h264";
    const fs::path seg_b = dir / "seg_b.h264";
    const fs::path combined = dir / "dynamic_res.h264";
    const std::string ffmpeg = FFMPEG_EXE_PATH;

    auto encode_segment = [&](const fs::path& out, const char* size) {
        std::ostringstream cmd;
        cmd << quote_arg(ffmpeg)
            << " -hide_banner -y -loglevel error"
            << " -f lavfi -i " << quote_arg(std::string("testsrc=size=") + size + ":rate=5:duration=1")
            << " -frames:v 5"
            << " -c:v libx264 -preset ultrafast -tune zerolatency"
            << " -g 5 -keyint_min 5 -x264-params " << quote_arg("scenecut=0:repeat-headers=1")
            << " -pix_fmt yuv420p -f h264 "
            << quote_arg(out.string());
        const int ret = run_command(cmd.str());
        REQUIRE(ret == 0);
        REQUIRE(fs::exists(out));
    };

    encode_segment(seg_a, "64x64");
    encode_segment(seg_b, "96x72");

    std::ofstream out(combined, std::ios::binary);
    REQUIRE(out.good());
    append_file(seg_a, out);
    append_file(seg_b, out);
    out.close();
    REQUIRE(fs::exists(combined));
    return combined;
}

} // namespace

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

TEST_CASE("DecodeThread: software decode follows generated H264 resolution changes",
          "[decode_thread][dynamic_resolution]") {
    const auto path = make_dynamic_resolution_h264_fixture();

    PacketQueue pkt_queue(100);
    TrackBuffer track_buffer(20, 2);
    SeekController sc;
    DemuxThread demux(path.string(), pkt_queue, sc);
    REQUIRE(demux.start());
    REQUIRE(demux.stats().video_stream_index >= 0);
    REQUIRE(demux.stats().codec_params != nullptr);

    DecodeThread decoder(pkt_queue, track_buffer, demux.stats().codec_params, demux.stats().time_base);
    REQUIRE(decoder.start());

    bool saw_first_size = false;
    bool saw_second_size = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline && !saw_second_size) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const size_t count = track_buffer.total_count();
        for (size_t i = 0; i < count; ++i) {
            auto frame = track_buffer.peek(static_cast<int>(i));
            if (!frame.has_value()) continue;
            saw_first_size = saw_first_size || (frame->width == 64 && frame->height == 64);
            saw_second_size = saw_second_size || (frame->width == 96 && frame->height == 72);
        }
    }

    decoder.stop();
    demux.stop();
    std::filesystem::remove_all(path.parent_path());

    REQUIRE(saw_first_size);
    REQUIRE(saw_second_size);
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
