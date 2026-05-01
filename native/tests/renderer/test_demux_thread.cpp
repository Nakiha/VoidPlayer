#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "media/demux_thread.h"
#include "media/seek_controller.h"
#include <thread>
#include <chrono>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <sstream>

using namespace vr;

namespace {

// Helper: drain N packets from the queue, returning them as a vector.
// Caller is responsible for freeing the packets.
std::vector<AVPacket*> drain_packets(PacketQueue& pq, int count) {
    std::vector<AVPacket*> packets;
    for (int i = 0; i < count; ++i) {
        auto* pkt = pq.pop();
        if (!pkt) break;
        packets.push_back(pkt);
    }
    return packets;
}

void free_packets(std::vector<AVPacket*>& packets) {
    for (auto* pkt : packets) {
        av_packet_free(&pkt);
    }
    packets.clear();
}

std::string get_h264_path() {
    return vr::test::video_test_dir() + "/h264_9s_1920x1080.mp4";
}

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
    // cmd.exe needs an extra pair of quotes when the executable path itself is quoted.
    return std::system(("\"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

std::string make_multi_audio_fixture() {
    namespace fs = std::filesystem;
    const fs::path output =
        fs::temp_directory_path() / "void_player_demux_multi_audio.mp4";
    const std::string ffmpeg = FFMPEG_EXE_PATH;
    std::ostringstream cmd;
    cmd << quote_arg(ffmpeg)
        << " -y -loglevel error"
        << " -t 2 -i " << quote_arg(get_h264_path())
        << " -f lavfi -i " << quote_arg("sine=frequency=440:duration=2")
        << " -f lavfi -i " << quote_arg("sine=frequency=880:duration=2")
        << " -map 0:v:0 -map 1:a:0 -map 2:a:0"
        << " -c:v copy -c:a aac -shortest "
        << quote_arg(output.string());

    const int ret = run_command(cmd.str());
    REQUIRE(ret == 0);
    REQUIRE(fs::exists(output));
    return output.string();
}

int count_streams(AVFormatContext* fmt, AVMediaType type) {
    int count = 0;
    if (!fmt) return count;
    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == type) {
            ++count;
        }
    }
    return count;
}

} // anonymous namespace

TEST_CASE("DemuxThread: open h264 file and verify stats", "[demux_thread]") {
    PacketQueue pq(200);
    SeekController sc;
    DemuxThread demux(get_h264_path(), pq, sc);

    REQUIRE(demux.start());

    // Give the demux thread a moment to begin reading
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto& s = demux.stats();
    REQUIRE(s.video_stream_index >= 0);
    REQUIRE(s.width == 1920);
    REQUIRE(s.height == 1080);

    // Duration should be approximately 9 seconds = 9,000,000 us.
    // Allow generous tolerance since container duration may differ slightly.
    REQUIRE(s.duration_us > 8000000);
    REQUIRE(s.duration_us < 10000000);

    REQUIRE(s.codec_params != nullptr);

    demux.stop();
}

TEST_CASE("DemuxThread: drained packets have correct stream_index",
          "[demux_thread]") {
    PacketQueue pq(200);
    SeekController sc;
    DemuxThread demux(get_h264_path(), pq, sc);

    REQUIRE(demux.start());

    // Wait for some packets to be enqueued
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto packets = drain_packets(pq, 20);

    // We should have received some packets
    REQUIRE(packets.size() >= 5);

    int expected_stream_index = demux.stats().video_stream_index;
    for (auto* pkt : packets) {
        REQUIRE(pkt->stream_index == expected_stream_index);
    }

    free_packets(packets);
    demux.stop();
}

TEST_CASE("DemuxThread: output routes are fixed after start",
          "[demux_thread]") {
    PacketQueue video_pq(200);
    PacketQueue audio_pq(200);
    SeekController sc;
    DemuxThread demux(get_h264_path(), video_pq, sc);

    REQUIRE(demux.start());
    REQUIRE_FALSE(demux.add_output(DemuxStreamKind::Audio, audio_pq));

    demux.stop();
}

TEST_CASE("DemuxThread: audio output receives only first audio stream",
          "[demux_thread]") {
    const std::string path = make_multi_audio_fixture();
    PacketQueue audio_pq(200);
    SeekController sc;
    DemuxThread demux(path, sc);
    REQUIRE(demux.add_output(DemuxStreamKind::Audio, audio_pq));

    REQUIRE(demux.start());
    REQUIRE(demux.stats().audio_stream_index >= 0);
    REQUIRE(demux.stats().audio_codec_params != nullptr);
    REQUIRE(demux.stats().sample_rate > 0);
    REQUIRE(demux.stats().channels > 0);
    REQUIRE(count_streams(demux.format_context(), AVMEDIA_TYPE_AUDIO) >= 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto packets = drain_packets(audio_pq, 20);
    REQUIRE(packets.size() >= 5);

    const int expected_stream_index = demux.stats().audio_stream_index;
    for (auto* pkt : packets) {
        REQUIRE(pkt->stream_index == expected_stream_index);
    }

    free_packets(packets);
    demux.stop();
    std::filesystem::remove(path);
}

TEST_CASE("DemuxThread: requires explicit output route",
          "[demux_thread]") {
    SeekController sc;
    DemuxThread demux(get_h264_path(), sc);

    REQUIRE_FALSE(demux.start());
    REQUIRE(demux.format_context() == nullptr);
}

TEST_CASE("DemuxThread: first packet has reasonable PTS", "[demux_thread]") {
    PacketQueue pq(200);
    SeekController sc;
    DemuxThread demux(get_h264_path(), pq, sc);

    REQUIRE(demux.start());

    // Pop the first packet
    auto* pkt = pq.pop();
    REQUIRE(pkt != nullptr);

    // PTS should be a reasonable microsecond value: >= 0 and < duration
    REQUIRE(pkt->pts != AV_NOPTS_VALUE);
    REQUIRE(pkt->pts >= 0);
    REQUIRE(pkt->pts < demux.stats().duration_us);

    av_packet_free(&pkt);
    demux.stop();
}

TEST_CASE("DemuxThread: stop cleans up without crash", "[demux_thread]") {
    PacketQueue pq(200);
    SeekController sc;
    {
        DemuxThread demux(get_h264_path(), pq, sc);
        REQUIRE(demux.start());

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        demux.stop();

        // After stop, format_context should be null
        REQUIRE(demux.format_context() == nullptr);
    }
    // Destructor also calls stop() — should not crash on double-stop
}
