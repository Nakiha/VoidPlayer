#include <catch2/catch_test_macros.hpp>
#include "analysis/generators/analysis_generator.h"
#include "analysis/generators/bitstream_indexer.h"
#include "analysis/analysis_manager.h"
#include "analysis/parsers/vbt_parser.h"
#include "analysis/parsers/vbi_parser.h"
#include "common/win_utf8.h"

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <vector>

static const std::string test_dir = VIDEO_TEST_DIR;
static const std::string h266_video = test_dir + "/h266_10s_1920x1080.mp4";

static bool video_exists() {
    return std::filesystem::exists(h266_video);
}

// Helper: generate VBI+VBT to temp files, verify, then clean up.
// Returns the temp directory path (caller should remove when done).
static std::string make_temp_dir() {
    auto dir = std::filesystem::temp_directory_path() / "void_player_test_gen";
    std::filesystem::create_directories(dir);
    return dir.string();
}

// ===========================================================================
// AnalysisGenerator: VBI + VBT generation
// ===========================================================================

TEST_CASE("AnalysisGenerator: generates VBI and VBT from H.266 MP4", "[analysis][generator]") {
    if (!video_exists()) return;

    auto tmp = make_temp_dir();
    std::string vbi_path = tmp + "/test.vbi";
    std::string vbt_path = tmp + "/test.vbt";

    REQUIRE(vr::analysis::AnalysisGenerator::generate(h266_video, vbi_path, vbt_path));

    // Verify VBT
    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(vbt_path));
    auto& vh = vbt.header();
    REQUIRE(vh.magic[0] == 'V');
    REQUIRE(vh.magic[1] == 'B');
    REQUIRE(vh.magic[2] == 'T');
    REQUIRE(vh.magic[3] == '1');
    REQUIRE(vbt.packet_count() == 600);
    REQUIRE(vh.time_base_num == 1);
    REQUIRE(vh.time_base_den == 60);

    // First packet should be a keyframe with PTS=0
    auto& e0 = vbt.entry(0);
    REQUIRE(e0.flags & VBT_FLAG_KEYFRAME);
    REQUIRE(e0.pts == 0);
    REQUIRE(e0.size > 0);

    // Verify VBI
    vr::analysis::VbiFile vbi;
    REQUIRE(vbi.open(vbi_path));
    auto& bh = vbi.header();
    REQUIRE(bh.magic[0] == 'V');
    REQUIRE(bh.magic[1] == 'B');
    REQUIRE(bh.magic[2] == 'I');
    REQUIRE(bh.magic[3] == '2');
    REQUIRE(bh.version == 2);
    REQUIRE(vbi.codec() == VbiCodec::VVC);
    REQUIRE(vbi.unit_kind() == VbiUnitKind::Nalu);
    REQUIRE(vbi.nalu_count() >= 600);
    REQUIRE(bh.source_size > 0);

    // First NALU from container: may be AUD(20), VPS(14), SPS(15), PPS(16), or slice
    // depending on how the container stores parameter sets (in extradata vs inline)
    uint8_t first_nal = vbi.entry(0).nal_type;
    REQUIRE(first_nal <= 31); // valid VVC NALU type range

    // VCL and keyframe counts should be reasonable
    auto vcl = vbi.find_vcl_nalus();
    auto kf = vbi.find_keyframes();
    REQUIRE(vcl.size() == 600);
    REQUIRE(kf.size() >= 1);

    // Cleanup
    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: VBT keyframe indices", "[analysis][generator]") {
    if (!video_exists()) return;

    auto tmp = make_temp_dir();
    std::string vbi_path = tmp + "/test.vbi";
    std::string vbt_path = tmp + "/test.vbt";

    REQUIRE(vr::analysis::AnalysisGenerator::generate(h266_video, vbi_path, vbt_path));

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(vbt_path));

    auto kf = vbt.keyframe_indices();
    REQUIRE(kf.size() == 10); // 10s at 60fps with 1s GOP

    // All keyframes should have the keyframe flag
    for (int idx : kf) {
        REQUIRE(vbt.entry(idx).flags & VBT_FLAG_KEYFRAME);
    }

    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: VBT durations", "[analysis][generator]") {
    if (!video_exists()) return;

    auto tmp = make_temp_dir();
    std::string vbi_path = tmp + "/test.vbi";
    std::string vbt_path = tmp + "/test.vbt";

    REQUIRE(vr::analysis::AnalysisGenerator::generate(h266_video, vbi_path, vbt_path));

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(vbt_path));

    // All packets should have positive size
    for (int i = 0; i < vbt.packet_count(); i++) {
        REQUIRE(vbt.entry(i).size > 0);
    }

    // Last PTS should be ~10s worth
    auto& last = vbt.entry(vbt.packet_count() - 1);
    double last_pts_time = static_cast<double>(last.pts) / vbt.header().time_base_den;
    REQUIRE(last_pts_time >= 9.0);
    REQUIRE(last_pts_time <= 10.5);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: VBI offsets monotonic", "[analysis][generator]") {
    if (!video_exists()) return;

    auto tmp = make_temp_dir();
    std::string vbi_path = tmp + "/test.vbi";
    std::string vbt_path = tmp + "/test.vbt";

    REQUIRE(vr::analysis::AnalysisGenerator::generate(h266_video, vbi_path, vbt_path));

    vr::analysis::VbiFile vbi;
    REQUIRE(vbi.open(vbi_path));

    for (int i = 1; i < vbi.nalu_count(); i++) {
        REQUIRE(vbi.entry(i).offset > vbi.entry(i - 1).offset);
        REQUIRE(vbi.entry(i).size > 0);
    }

    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: VBI VCL flags match nal_type", "[analysis][generator]") {
    if (!video_exists()) return;

    auto tmp = make_temp_dir();
    std::string vbi_path = tmp + "/test.vbi";
    std::string vbt_path = tmp + "/test.vbt";

    REQUIRE(vr::analysis::AnalysisGenerator::generate(h266_video, vbi_path, vbt_path));

    vr::analysis::VbiFile vbi;
    REQUIRE(vbi.open(vbi_path));

    // VCL types are 0-11; non-VCL types are 12+
    for (int i = 0; i < vbi.nalu_count(); i++) {
        auto& e = vbi.entry(i);
        if (e.nal_type <= 11) {
            REQUIRE(e.flags & 0x01); // VBI_FLAG_IS_VCL
        } else {
            REQUIRE((e.flags & 0x01) == 0);
        }
    }

    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: nonexistent input returns false", "[analysis][generator]") {
    auto tmp = make_temp_dir();
    std::string vbi_path = tmp + "/test.vbi";
    std::string vbt_path = tmp + "/test.vbt";

    REQUIRE_FALSE(vr::analysis::AnalysisGenerator::generate(
        "/nonexistent/file.mp4", vbi_path, vbt_path));

    // Output files should not exist
    REQUIRE_FALSE(std::filesystem::exists(vbi_path));
    REQUIRE_FALSE(std::filesystem::exists(vbt_path));

    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: accepts UTF-8 paths with non-ASCII characters",
          "[analysis][generator][unicode]") {
    namespace fs = std::filesystem;
    const fs::path source = fs::path(test_dir) / "h264_9s_1920x1080.mp4";
    if (!fs::exists(source)) return;

    const fs::path tmp =
        fs::temp_directory_path() / fs::u8path("void_player_unicode_路径_テスト");
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    const fs::path video_path = tmp / fs::u8path("输入_動画.mp4");
    const fs::path vbi_path = tmp / fs::u8path("结果.vbi");
    const fs::path vbt_path = tmp / fs::u8path("结果.vbt");
    fs::copy_file(source, video_path, fs::copy_options::overwrite_existing);

    REQUIRE(vr::analysis::AnalysisGenerator::generate(
        vr::win_utf8::path_to_utf8(video_path),
        vr::win_utf8::path_to_utf8(vbi_path),
        vr::win_utf8::path_to_utf8(vbt_path)));

    vr::analysis::VbiFile vbi;
    REQUIRE(vbi.open(vr::win_utf8::path_to_utf8(vbi_path)));
    REQUIRE(vbi.nalu_count() > 0);

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(vr::win_utf8::path_to_utf8(vbt_path)));
    REQUIRE(vbt.packet_count() > 0);

    fs::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: resources video samples produce VBI2 and VBT", "[analysis][generator][resources]") {
    struct SampleCase {
        const char* name;
        VbiCodec codec;
        VbiUnitKind unit_kind;
        int min_units;
        int expected_packets;
    };

    const std::vector<SampleCase> samples = {
        {"av1_10s_1920x1080.webm",       VbiCodec::AV1,   VbiUnitKind::Obu,       600, 600},
        {"h264_9s_1920x1080.mp4",        VbiCodec::H264,  VbiUnitKind::Nalu,      600, 600},
        {"h265_10s_1920x1080.mp4",       VbiCodec::HEVC,  VbiUnitKind::Nalu,      600, 600},
        {"h266_10s_1920x1080_lbp.vvc",   VbiCodec::VVC,   VbiUnitKind::Nalu,      600, 600},
        {"h266_10s_1920x1080.mp4",       VbiCodec::VVC,   VbiUnitKind::Nalu,      600, 600},
        {"h266_10s_1920x1080.vvc",       VbiCodec::VVC,   VbiUnitKind::Nalu,      600, 600},
        {"mpeg2_10s_1280x720.ts",        VbiCodec::MPEG2, VbiUnitKind::StartCode, 600, 600},
        {"vp9_10s_1920x1080.webm",       VbiCodec::VP9,   VbiUnitKind::Packet,    600, 600},
    };

    auto tmp = make_temp_dir();

    for (const auto& sample : samples) {
        const std::string video = test_dir + "/" + sample.name;
        if (!std::filesystem::exists(video)) continue;

        const std::string base = std::string(sample.name);
        const std::string vbi_path = tmp + "/" + base + ".vbi";
        const std::string vbt_path = tmp + "/" + base + ".vbt";

        REQUIRE(vr::analysis::AnalysisGenerator::generate(video, vbi_path, vbt_path));

        vr::analysis::VbiFile vbi;
        REQUIRE(vbi.open(vbi_path));
        REQUIRE(vbi.header().magic[3] == '2');
        REQUIRE(vbi.codec() == sample.codec);
        REQUIRE(vbi.unit_kind() == sample.unit_kind);
        REQUIRE(vbi.unit_count() >= sample.min_units);
        REQUIRE(vbi.header().source_size > 0);

        vr::analysis::VbtFile vbt;
        REQUIRE(vbt.open(vbt_path));
        REQUIRE(vbt.packet_count() == sample.expected_packets);
        for (int i = 0; i < vbt.packet_count(); i++) {
            REQUIRE(vbt.entry(i).size > 0);
        }
    }

    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisGenerator: MP4 parameter sets are indexed", "[analysis][generator][resources]") {
    struct SampleCase {
        const char* name;
        VbiCodec codec;
        std::vector<uint8_t> required_nal_types;
    };

    const std::vector<SampleCase> samples = {
        {"h264_9s_1920x1080.mp4",  VbiCodec::H264, {7, 8}},
        {"h265_10s_1920x1080.mp4", VbiCodec::HEVC, {32, 33, 34}},
    };

    auto tmp = make_temp_dir();

    for (const auto& sample : samples) {
        const std::string video = test_dir + "/" + sample.name;
        if (!std::filesystem::exists(video)) continue;

        const std::string base = std::string(sample.name);
        const std::string vbi_path = tmp + "/" + base + ".vbi";
        const std::string vbt_path = tmp + "/" + base + ".vbt";

        REQUIRE(vr::analysis::AnalysisGenerator::generate(video, vbi_path, vbt_path));

        vr::analysis::VbiFile vbi;
        REQUIRE(vbi.open(vbi_path));
        REQUIRE(vbi.codec() == sample.codec);

        for (uint8_t required_type : sample.required_nal_types) {
            bool found = false;
            for (int i = 0; i < vbi.nalu_count(); ++i) {
                if (vbi.entry(i).nal_type == required_type) {
                    found = true;
                    break;
                }
            }
            REQUIRE(found);
        }
    }

    std::filesystem::remove_all(tmp);
}

TEST_CASE("AnalysisManager: current frame handles high-denominator time bases", "[analysis][manager][resources]") {
    const std::string h265_video = test_dir + "/h265_10s_1920x1080.mp4";
    if (!std::filesystem::exists(h265_video)) return;

    auto tmp = make_temp_dir();
    const std::string vbi_path = tmp + "/h265.vbi";
    const std::string vbt_path = tmp + "/h265.vbt";

    REQUIRE(vr::analysis::AnalysisGenerator::generate(h265_video, vbi_path, vbt_path));

    auto& mgr = vr::analysis::AnalysisManager::instance();
    REQUIRE(mgr.load("", vbi_path, vbt_path));
    REQUIRE(mgr.vbt().header().time_base_den > 1000000);

    REQUIRE(mgr.current_frame_idx(0) >= 0);
    REQUIRE(mgr.current_frame_idx(1000000) >= 0);

    mgr.unload();
    std::filesystem::remove_all(tmp);
}

TEST_CASE("BitstreamIndexer: converts length-prefixed VVC sample to Annex-B", "[analysis][generator][resources]") {
    const std::string lbp_video = test_dir + "/h266_10s_1920x1080_lbp.vvc";
    if (!std::filesystem::exists(lbp_video)) return;

    auto tmp = make_temp_dir();
    const std::string annex_b = tmp + "/lbp_annexb.vvc";
    REQUIRE(vr::analysis::BitstreamIndexer::write_annex_b_file(
        lbp_video, VbiCodec::VVC, annex_b));

    {
        std::ifstream in(annex_b, std::ios::binary);
        REQUIRE(in.good());
        char start_code[4] = {};
        in.read(start_code, sizeof(start_code));
        REQUIRE(start_code[0] == 0);
        REQUIRE(start_code[1] == 0);
        REQUIRE(start_code[2] == 0);
        REQUIRE(start_code[3] == 1);
    }

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
        raw_path.string(), VbiCodec::H264, index));
    REQUIRE(index.entries.size() == 2);
    REQUIRE(index.entries[0].offset == 0);
    REQUIRE(index.entries[0].nal_type == 7);
    REQUIRE(index.entries[1].nal_type == 5);
    REQUIRE(index.entries[1].flags & VBI_FLAG_IS_KEYFRAME);

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
    VbiCodec resolved = VbiCodec::Unknown;
    REQUIRE(vr::analysis::BitstreamIndexer::index_raw_file_streaming(
        raw_path.string(),
        VbiCodec::HEVC,
        [&](const VbiEntry& entry) {
            if (count == 0) {
                REQUIRE(entry.offset == 0);
                REQUIRE(entry.nal_type == 32);
            }
            if (count == 1) {
                REQUIRE(entry.nal_type == 19);
                REQUIRE(entry.flags & VBI_FLAG_IS_KEYFRAME);
            }
            ++count;
            return true;
        },
        &resolved,
        &source_size));
    REQUIRE(resolved == VbiCodec::HEVC);
    REQUIRE(count == 2);
    REQUIRE(source_size == fs::file_size(raw_path));

    std::filesystem::remove_all(tmp);
}
