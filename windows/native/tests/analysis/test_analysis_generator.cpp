#include <catch2/catch_test_macros.hpp>
#include "analysis/generators/analysis_generator.h"
#include "analysis/parsers/vbt_parser.h"
#include "analysis/parsers/vbi_parser.h"

#include <filesystem>
#include <fstream>
#include <cstdio>

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
