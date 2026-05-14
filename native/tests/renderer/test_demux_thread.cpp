#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "common/win_utf8.h"
#include "media/demux_thread.h"
#include "media/seek_controller.h"
#include <thread>
#include <chrono>
#include <string>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <sstream>

using namespace vr;

namespace {

// Helper: drain N packets from the queue, returning them as a vector.
// Caller is responsible for freeing the packets.
std::vector<AVPacket*> drain_packets(PacketQueue& pq, int count) {
    std::vector<AVPacket*> packets;
    for (int i = 0; i < count; ++i) {
        auto result = pq.pop();
        auto* pkt = result.packet;
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

void append_u24(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_private_video_tag(std::vector<uint8_t>& out, uint8_t codec_id,
                              uint32_t timestamp_ms, uint8_t packet_type,
                              uint32_t cts, const std::vector<uint8_t>& payload) {
    const uint32_t data_size = 1 + 1 + 3 + static_cast<uint32_t>(payload.size());
    out.push_back(0x09);
    append_u24(out, data_size);
    append_u24(out, timestamp_ms & 0x00ffffff);
    out.push_back(static_cast<uint8_t>((timestamp_ms >> 24) & 0xff));
    append_u24(out, 0);
    out.push_back(static_cast<uint8_t>(0x10 | codec_id)); // keyframe + private codec id
    out.push_back(packet_type);
    append_u24(out, cts);
    out.insert(out.end(), payload.begin(), payload.end());
    append_u32(out, data_size + 11);
}

std::string make_private_flv_fixture(uint8_t codec_id, const char* name) {
    namespace fs = std::filesystem;
    const fs::path output = fs::temp_directory_path() / name;
    const std::vector<uint8_t> vvc_config = {
        0x07,             // ptl_present=1, lengthSizeMinusOne=3 (4-byte NAL lengths)
        0x00, 0x11,       // numTemporalLayers=1, chroma_format_idc=1 (4:2:0)
        0x40,             // bit_depth_minus8=2 (10-bit)
        0x01,             // num_bytes_constraint_info=1
        0x00, 0x00, 0x00, // profile/tier, level, constraint byte
        0x00,             // num_sub_profiles
        0x07, 0x80,       // max_picture_width=1920
        0x04, 0x38,       // max_picture_height=1080
        0x00, 0x00,       // avg_frame_rate
        0x00,             // numOfArrays
    };
    std::vector<uint8_t> bytes = {
        'F', 'L', 'V', 0x01, 0x01,
        0x00, 0x00, 0x00, 0x09,
        0x00, 0x00, 0x00, 0x00,
    };
    append_private_video_tag(bytes, codec_id, 0, 0, 0,
                             codec_id == 0x0d
                                 ? std::vector<uint8_t>{0x81, 0x00, 0x00, 0x00, 0x12, 0x34}
                                 : vvc_config);
    append_private_video_tag(bytes, codec_id, 40, 1, 5, {0xaa, 0xbb, 0xcc});

    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
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

TEST_CASE("DemuxThread: copied stats own codec parameters after stop",
          "[demux_thread]") {
    PacketQueue pq(200);
    SeekController sc;
    DemuxStats copied_stats;

    {
        DemuxThread demux(get_h264_path(), pq, sc);
        REQUIRE(demux.start());
        copied_stats = demux.stats();
        REQUIRE(copied_stats.codec_params != nullptr);
        REQUIRE(copied_stats.codec_params == copied_stats.codec_params_owner.get());
        demux.stop();
    }

    REQUIRE(copied_stats.codec_params != nullptr);
    REQUIRE(copied_stats.codec_params == copied_stats.codec_params_owner.get());
    REQUIRE(copied_stats.codec_params->codec_type == AVMEDIA_TYPE_VIDEO);
    REQUIRE(copied_stats.width == 1920);
    REQUIRE(copied_stats.height == 1080);
}

TEST_CASE("DemuxThread: opens UTF-8 paths with non-ASCII characters",
          "[demux_thread][unicode]") {
    namespace fs = std::filesystem;
    const fs::path source = fs::path(get_h264_path());
    if (!fs::exists(source)) return;

    const fs::path tmp =
        fs::temp_directory_path() / fs::u8path("void_player_demux_unicode_路径_テスト");
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const fs::path video_path = tmp / fs::u8path("输入_動画.mp4");
    fs::copy_file(source, video_path, fs::copy_options::overwrite_existing);

    PacketQueue pq(200);
    SeekController sc;
    DemuxThread demux(vr::win_utf8::path_to_utf8(video_path), pq, sc);

    REQUIRE(demux.start());
    REQUIRE(demux.stats().video_stream_index >= 0);
    REQUIRE(demux.stats().width == 1920);
    REQUIRE(demux.stats().height == 1080);

    demux.stop();
    fs::remove_all(tmp);
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

TEST_CASE("DemuxThread: private CDN FLV AV1 fallback emits packets",
          "[demux_thread][flv]") {
    const std::string path = make_private_flv_fixture(0x0d, "void_player_private_cdn_av1.flv");
    PacketQueue pq(8);
    SeekController sc;
    DemuxThread demux(path, pq, sc);

    REQUIRE(demux.start());
    REQUIRE(demux.format_context() == nullptr);
    REQUIRE(demux.stats().video_stream_index == 0);
    REQUIRE(demux.stats().codec_params != nullptr);
    REQUIRE(demux.stats().codec_params->codec_id == AV_CODEC_ID_AV1);
    REQUIRE(demux.stats().codec_params->extradata_size == 2);

    auto packet_result = pq.pop();
    REQUIRE(packet_result.status == PacketPopStatus::Packet);
    AVPacket* pkt = packet_result.packet;
    REQUIRE(pkt != nullptr);
    REQUIRE(pkt->stream_index == demux.stats().video_stream_index);
    REQUIRE(pkt->dts == 40);
    REQUIRE(pkt->pts == 45);
    REQUIRE(pkt->size == 3);
    REQUIRE((pkt->flags & AV_PKT_FLAG_KEY) != 0);

    av_packet_free(&pkt);
    demux.stop();
    std::filesystem::remove(path);
}

TEST_CASE("DemuxThread: private CDN FLV VVC fallback emits packets",
          "[demux_thread][flv]") {
    const std::string path = make_private_flv_fixture(0x0e, "void_player_private_cdn_vvc.flv");
    PacketQueue pq(8);
    SeekController sc;
    DemuxThread demux(path, pq, sc);

    REQUIRE(demux.start());
    REQUIRE(demux.format_context() == nullptr);
    REQUIRE(demux.stats().codec_params != nullptr);
    REQUIRE(demux.stats().codec_params->codec_id == AV_CODEC_ID_VVC);
    REQUIRE(demux.stats().codec_params->extradata_size > 0);
    REQUIRE(demux.stats().width == 1920);
    REQUIRE(demux.stats().height == 1080);
    REQUIRE(demux.stats().codec_params->format == AV_PIX_FMT_YUV420P10LE);

    auto packet_result = pq.pop();
    REQUIRE(packet_result.status == PacketPopStatus::Packet);
    AVPacket* pkt = packet_result.packet;
    REQUIRE(pkt != nullptr);
    REQUIRE(pkt->dts == 40);
    REQUIRE(pkt->pts == 45);
    REQUIRE(pkt->size == 3);

    av_packet_free(&pkt);
    demux.stop();
    std::filesystem::remove(path);
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

TEST_CASE("DemuxThread: two-stage start keeps pending seek until callback is wired",
          "[demux_thread][seek]") {
    PacketQueue video_pq(200);
    PacketQueue audio_pq(200);
    SeekController sc;
    sc.request_seek(1000000, SeekType::Exact);

    DemuxThread demux(get_h264_path(), video_pq, sc);

    REQUIRE(demux.open());
    REQUIRE(sc.has_pending_seek());
    REQUIRE_FALSE(demux.add_output(DemuxStreamKind::Audio, audio_pq));

    std::atomic<int> callback_count{0};
    std::atomic<int64_t> callback_pts{-1};
    std::atomic<int> callback_type{-1};
    demux.set_seek_callback([&](int64_t pts, SeekType type) {
        callback_pts.store(pts, std::memory_order_release);
        callback_type.store(static_cast<int>(type), std::memory_order_release);
        callback_count.fetch_add(1, std::memory_order_acq_rel);
    });

    REQUIRE(demux.start_thread());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (callback_count.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(callback_count.load(std::memory_order_acquire) == 1);
    REQUIRE(callback_pts.load(std::memory_order_acquire) == 1000000);
    REQUIRE(callback_type.load(std::memory_order_acquire) == static_cast<int>(SeekType::Exact));

    demux.stop();
}

TEST_CASE("DemuxThread: read errors invoke callback and abort queues",
          "[demux_thread]") {
    PacketQueue video_pq;
    SeekController sc;
    DemuxThread demux(get_h264_path(), video_pq, sc);

    std::atomic<int> error_code{0};
    demux.set_error_callback([&](int code) {
        error_code.store(code, std::memory_order_release);
    });

    REQUIRE(demux.open());
    demux.fail_next_read_for_test(AVERROR_INVALIDDATA);
    REQUIRE(demux.start_thread());

    for (int i = 0; i < 100; ++i) {
        if (error_code.load(std::memory_order_acquire) == AVERROR_INVALIDDATA &&
            video_pq.is_aborted()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(error_code.load(std::memory_order_acquire) == AVERROR_INVALIDDATA);
    REQUIRE(video_pq.is_aborted());

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
    auto packet_result = pq.pop();
    REQUIRE(packet_result.status == PacketPopStatus::Packet);
    auto* pkt = packet_result.packet;
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

TEST_CASE("DemuxThread: stop can race with start without leaking context",
          "[demux_thread][concurrency]") {
    for (int i = 0; i < 20; ++i) {
        PacketQueue pq(8);
        SeekController sc;
        DemuxThread demux(get_h264_path(), pq, sc);

        bool started = false;
        std::thread starter([&] {
            started = demux.start();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        demux.stop();
        starter.join();
        if (started) {
            demux.stop();
        }
        REQUIRE(demux.format_context() == nullptr);
    }
}
