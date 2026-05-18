#include <catch2/catch_test_macros.hpp>

#include "analysis/analysis_manager.h"
#include "analysis/generators/analysis_generator.h"
#include "analysis/generators/bitstream_indexer.h"
#include "analysis/parsers/vac2_parser.h"
#include "common/win_utf8.h"

#include <filesystem>
#include <fstream>
#include <vector>

static const std::string test_dir = VIDEO_TEST_DIR;

static std::string make_temp_dir() {
    auto dir = std::filesystem::temp_directory_path() / "void_player_test_gen";
    std::filesystem::create_directories(dir);
    return dir.string();
}

static void append_u24(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

static void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

static void append_private_flv_video_tag(std::vector<uint8_t>& out,
                                         uint8_t codec_id,
                                         uint32_t timestamp_ms,
                                         uint8_t packet_type,
                                         uint32_t cts,
                                         const std::vector<uint8_t>& payload) {
    const uint32_t data_size = 1 + 1 + 3 + static_cast<uint32_t>(payload.size());
    out.push_back(0x09);
    append_u24(out, data_size);
    append_u24(out, timestamp_ms & 0x00ffffff);
    out.push_back(static_cast<uint8_t>((timestamp_ms >> 24) & 0xff));
    append_u24(out, 0);
    out.push_back(static_cast<uint8_t>(0x10 | codec_id));
    out.push_back(packet_type);
    append_u24(out, cts);
    out.insert(out.end(), payload.begin(), payload.end());
    append_u32(out, data_size + 11);
}

static std::string make_private_av1_flv_fixture(const std::string& dir) {
    const auto path = std::filesystem::path(dir) / "private_av1.flv";
    std::vector<uint8_t> bytes = {
        'F', 'L', 'V', 0x01, 0x01,
        0x00, 0x00, 0x00, 0x09,
        0x00, 0x00, 0x00, 0x00,
    };
    append_private_flv_video_tag(
        bytes, 0x0d, 0, 0, 0,
        {0x81, 0x00, 0x00, 0x00, 0x12, 0x34});
    append_private_flv_video_tag(
        bytes, 0x0d, 40, 1, 5,
        {0x32, 0x01, 0x00});

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    file.close();
    return path.string();
}

TEST_CASE("AnalysisGenerator: generates VAC2 base from private CDN FLV",
          "[analysis][generator][vac2][flv]") {
    auto tmp = make_temp_dir();
    const std::string video_path = make_private_av1_flv_fixture(tmp);
    const std::string vac2_path = tmp + "/base.vac";

    REQUIRE(vr::analysis::AnalysisGenerator::generate_vac2_base(video_path, vac2_path));

    vr::analysis::Vac2BaseFile vac2;
    REQUIRE(vac2.open(vac2_path));
    REQUIRE(vac2.header().magic[0] == 'V');
    REQUIRE(vac2.header().magic[1] == 'A');
    REQUIRE(vac2.header().magic[2] == 'C');
    REQUIRE(vac2.header().magic[3] == '2');
    REQUIRE(vac2.header().codec == static_cast<uint16_t>(AnalysisCodec::AV1));
    REQUIRE(vac2.header().packet_count == 1);
    REQUIRE(vac2.header().unit_count == 1);
    REQUIRE(vac2.header().au_count == 1);
    REQUIRE(vac2.metadata_json().find(
                "\"frame_model\":\"one_packet_per_frame_fallback\"") !=
            std::string::npos);
    REQUIRE(vac2.packets()[0].pts == 45);
    REQUIRE(vac2.packets()[0].dts == 40);
    REQUIRE(vac2.packets()[0].au_index == 0);
    REQUIRE(vac2.units()[0].nal_type == 6);
    REQUIRE(vac2.units()[0].flags & VAC2_UNIT_FLAG_IS_KEYFRAME);
    REQUIRE(vac2.units()[0].au_index == 0);
    REQUIRE(vac2.frames()[0].first_packet == 0);
    REQUIRE(vac2.frames()[0].packet_count == 1);
    REQUIRE(vac2.frames()[0].flags & VAC2_FRAME_FLAG_INFERRED_AU);
    REQUIRE(vac2.frames()[0].frame_size > 0);
    REQUIRE(vac2.frame_summaries()[0].flags &
            VAC2_FRAME_SUMMARY_FLAG_INFERRED_AU);
    REQUIRE(vac2.frame_summaries()[0].qp_kind == VAC2_QP_KIND_UNKNOWN);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: H.265 VAC2 base infers fallback reference edges",
          "[analysis][generator][vac2][resources][refs]") {
    const std::string h265_video = test_dir + "/h265_10s_1920x1080.mp4";
    if (!std::filesystem::exists(h265_video)) return;

    auto tmp = make_temp_dir();
    const std::string vac2_path = tmp + "/h265_refs.vac";
    REQUIRE(vr::analysis::AnalysisGenerator::generate_vac2_base(
        h265_video, vac2_path));

    vr::analysis::Vac2BaseFile vac2;
    REQUIRE(vac2.open(vac2_path));
    REQUIRE(vac2.header().codec == static_cast<uint16_t>(AnalysisCodec::HEVC));

    int ref_edges = 0;
    for (const auto& summary : vac2.frame_summaries()) {
        for (uint8_t i = 0; i < summary.num_ref_l0 && i < 15; ++i) {
            if (summary.ref_pocs_l0[i] >= 0) ++ref_edges;
        }
        for (uint8_t i = 0; i < summary.num_ref_l1 && i < 15; ++i) {
            if (summary.ref_pocs_l1[i] >= 0) ++ref_edges;
        }
    }

    REQUIRE(ref_edges > 0);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: resources video samples produce VAC2 base",
          "[analysis][generator][vac2][resources]") {
    struct SampleCase {
        const char* name;
        AnalysisCodec codec;
        AnalysisUnitKind unit_kind;
        int min_units;
        int expected_packets;
    };

    const std::vector<SampleCase> samples = {
        {"av1_10s_1920x1080.webm",       AnalysisCodec::AV1,   AnalysisUnitKind::Obu,       600, 600},
        {"h264_9s_1920x1080.mp4",        AnalysisCodec::H264,  AnalysisUnitKind::Nalu,      600, 600},
        {"h265_10s_1920x1080.mp4",       AnalysisCodec::HEVC,  AnalysisUnitKind::Nalu,      600, 600},
        {"h266_10s_1920x1080_lbp.vvc",   AnalysisCodec::VVC,   AnalysisUnitKind::Nalu,      600, 600},
        {"h266_10s_1920x1080.mp4",       AnalysisCodec::VVC,   AnalysisUnitKind::Nalu,      600, 600},
        {"h266_10s_1920x1080.vvc",       AnalysisCodec::VVC,   AnalysisUnitKind::Nalu,      600, 600},
        {"mpeg2_10s_1280x720.ts",        AnalysisCodec::MPEG2, AnalysisUnitKind::StartCode, 600, 600},
        {"vp9_10s_1920x1080.webm",       AnalysisCodec::VP9,   AnalysisUnitKind::Packet,    600, 600},
    };

    auto tmp = make_temp_dir();
    for (const auto& sample : samples) {
        const std::string video = test_dir + "/" + sample.name;
        if (!std::filesystem::exists(video)) continue;

        const std::string vac2_path = tmp + "/" + std::string(sample.name) + ".vac";
        REQUIRE(vr::analysis::AnalysisGenerator::generate_vac2_base(video, vac2_path));

        vr::analysis::Vac2BaseFile vac2;
        REQUIRE(vac2.open(vac2_path));
        REQUIRE(vac2.header().codec == static_cast<uint16_t>(sample.codec));
        REQUIRE(vac2.units().size() >= static_cast<size_t>(sample.min_units));
        REQUIRE(vac2.packets().size() == static_cast<size_t>(sample.expected_packets));
        REQUIRE(vac2.frames().size() == static_cast<size_t>(sample.expected_packets));
        REQUIRE(vac2.units()[0].unit_kind == static_cast<uint8_t>(sample.unit_kind));
        for (size_t i = 0; i < vac2.packets().size(); ++i) {
            const auto& packet = vac2.packets()[i];
            REQUIRE(packet.size > 0);
            REQUIRE(packet.au_index == i);
            REQUIRE(vac2.frames()[i].first_packet == i);
            REQUIRE(vac2.frames()[i].packet_count == 1);
            REQUIRE(vac2.frames()[i].flags & VAC2_FRAME_FLAG_INFERRED_AU);
        }
    }
    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: nonexistent VAC2 input returns false",
          "[analysis][generator][vac2]") {
    auto tmp = make_temp_dir();
    const std::string vac2_path = tmp + "/missing.vac";
    REQUIRE_FALSE(vr::analysis::AnalysisGenerator::generate_vac2_base(
        "/nonexistent/file.mp4", vac2_path));
    REQUIRE_FALSE(std::filesystem::exists(vac2_path));
    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: VAC2 accepts UTF-8 paths with non-ASCII characters",
          "[analysis][generator][vac2][unicode]") {
    namespace fs = std::filesystem;
    const fs::path source = fs::path(test_dir) / "h264_9s_1920x1080.mp4";
    if (!fs::exists(source)) return;

    const fs::path tmp =
        fs::temp_directory_path() / fs::u8path("void_player_unicode_路径_テスト");
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const fs::path video_path = tmp / fs::u8path("输入_動画.mp4");
    const fs::path vac2_path = tmp / fs::u8path("结果.vac");
    fs::copy_file(source, video_path, fs::copy_options::overwrite_existing);

    REQUIRE(vr::analysis::AnalysisGenerator::generate_vac2_base(
        vr::win_utf8::path_to_utf8(video_path),
        vr::win_utf8::path_to_utf8(vac2_path)));

    vr::analysis::Vac2BaseFile vac2;
    REQUIRE(vac2.open(vr::win_utf8::path_to_utf8(vac2_path)));
    REQUIRE(!vac2.packets().empty());
    REQUIRE(!vac2.units().empty());

    fs::remove_all(tmp);
}

TEST_CASE("AnalysisManager: current frame handles high-denominator time bases",
          "[analysis][manager][resources]") {
    const std::string h265_video = test_dir + "/h265_10s_1920x1080.mp4";
    if (!std::filesystem::exists(h265_video)) return;

    auto tmp = make_temp_dir();
    const std::string vac_path = tmp + "/base.vac";

    REQUIRE(vr::analysis::AnalysisGenerator::generate_vac2_base(h265_video, vac_path));

    auto& mgr = vr::analysis::AnalysisManager::instance();
    REQUIRE(mgr.load(vac_path));
    vr::analysis::Vac2BaseFile vac2;
    REQUIRE(vac2.open(vac_path));
    REQUIRE(vac2.header().time_base_den > 1000000);
    REQUIRE(mgr.current_frame_idx(0) >= 0);
    REQUIRE(mgr.current_frame_idx(1000000) >= 0);

    mgr.unload();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("BitstreamIndexer: converts length-prefixed VVC sample to Annex-B",
          "[analysis][generator][resources]") {
    const std::string lbp_video = test_dir + "/h266_10s_1920x1080_lbp.vvc";
    if (!std::filesystem::exists(lbp_video)) return;

    auto tmp = make_temp_dir();
    const std::string annex_b = tmp + "/lbp_annexb.vvc";
    REQUIRE(vr::analysis::BitstreamIndexer::write_annex_b_file(
        lbp_video, AnalysisCodec::VVC, annex_b));

    std::ifstream in(annex_b, std::ios::binary);
    REQUIRE(in.good());
    char start_code[4] = {};
    in.read(start_code, sizeof(start_code));
    REQUIRE(start_code[0] == 0);
    REQUIRE(start_code[1] == 0);
    REQUIRE(start_code[2] == 0);
    REQUIRE(start_code[3] == 1);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("BitstreamIndexer: streams Annex-B raw files across chunk boundaries",
          "[analysis][generator][streaming]") {
    namespace fs = std::filesystem;
    auto tmp = make_temp_dir();
    const fs::path raw_path = fs::path(tmp) / "chunk_boundary.h264";

    {
        std::ofstream out(raw_path, std::ios::binary);
        REQUIRE(out.good());
        const std::vector<uint8_t> first = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f};
        out.write(reinterpret_cast<const char*>(first.data()),
                  static_cast<std::streamsize>(first.size()));
        std::vector<uint8_t> filler(70 * 1024, 0x55);
        out.write(reinterpret_cast<const char*>(filler.data()),
                  static_cast<std::streamsize>(filler.size()));
        const std::vector<uint8_t> second = {0, 0, 0, 1, 0x65, 0x88, 0x84};
        out.write(reinterpret_cast<const char*>(second.data()),
                  static_cast<std::streamsize>(second.size()));
    }

    vr::analysis::BitstreamIndex index;
    REQUIRE(vr::analysis::BitstreamIndexer::index_raw_file(
        raw_path.string(), AnalysisCodec::H264, index));
    REQUIRE(index.entries.size() == 2);
    REQUIRE(index.entries[0].offset == 0);
    REQUIRE(index.entries[0].nal_type == 7);
    REQUIRE(index.entries[1].nal_type == 5);
    REQUIRE(index.entries[1].flags & ANALYSIS_UNIT_FLAG_IS_KEYFRAME);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("BitstreamIndexer: streams raw entries through callback",
          "[analysis][generator][streaming]") {
    namespace fs = std::filesystem;
    auto tmp = make_temp_dir();
    const fs::path raw_path = fs::path(tmp) / "callback_stream.h265";

    {
        std::ofstream out(raw_path, std::ios::binary);
        REQUIRE(out.good());
        const std::vector<uint8_t> first = {0, 0, 0, 1, 0x40, 0x01, 0x0c};
        const std::vector<uint8_t> second = {0, 0, 0, 1, 0x26, 0x01, 0xaf};
        out.write(reinterpret_cast<const char*>(first.data()),
                  static_cast<std::streamsize>(first.size()));
        out.write(reinterpret_cast<const char*>(second.data()),
                  static_cast<std::streamsize>(second.size()));
    }

    size_t count = 0;
    uint64_t source_size = 0;
    AnalysisCodec resolved = AnalysisCodec::Unknown;
    REQUIRE(vr::analysis::BitstreamIndexer::index_raw_file_streaming(
        raw_path.string(),
        AnalysisCodec::HEVC,
        [&](const AnalysisUnitScanEntry& entry) {
            if (count == 0) {
                REQUIRE(entry.offset == 0);
                REQUIRE(entry.nal_type == 32);
            }
            if (count == 1) {
                REQUIRE(entry.nal_type == 19);
                REQUIRE(entry.flags & ANALYSIS_UNIT_FLAG_IS_KEYFRAME);
            }
            ++count;
            return true;
        },
        &resolved,
        &source_size));
    REQUIRE(resolved == AnalysisCodec::HEVC);
    REQUIRE(count == 2);
    REQUIRE(source_size == fs::file_size(raw_path));

    std::filesystem::remove_all(tmp);
}
