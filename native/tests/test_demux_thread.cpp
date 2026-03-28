#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/decode/demux_thread.h"
#include <thread>
#include <chrono>
#include <string>

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

} // anonymous namespace

TEST_CASE("DemuxThread: open h264 file and verify stats", "[demux_thread]") {
    PacketQueue pq(200);
    DemuxThread demux(get_h264_path(), pq);

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
    DemuxThread demux(get_h264_path(), pq);

    REQUIRE(demux.start());

    // Wait for some packets to be enqueued
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto packets = drain_packets(pq, 20);

    // We should have received some packets
    REQUIRE(packets.size() >= 5);

    int expected_stream_index = demux.stats().video_stream_index;
    for (auto* pkt : packets) {
        // The demux thread converts pts/dts to microseconds and stores them
        // back in the packet fields. The original stream_index filtering was
        // done inside the demux loop, but av_packet_unref already happened.
        // We just verify the stream_index on the packet (it was not modified).
        REQUIRE(pkt->stream_index == expected_stream_index);
    }

    free_packets(packets);
    demux.stop();
}

TEST_CASE("DemuxThread: first packet has reasonable PTS", "[demux_thread]") {
    PacketQueue pq(200);
    DemuxThread demux(get_h264_path(), pq);

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
    {
        DemuxThread demux(get_h264_path(), pq);
        REQUIRE(demux.start());

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        demux.stop();

        // After stop, format_context should be null
        REQUIRE(demux.format_context() == nullptr);
    }
    // Destructor also calls stop() — should not crash on double-stop
}
