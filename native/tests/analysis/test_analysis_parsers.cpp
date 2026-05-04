#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "analysis/parsers/analysis_container.h"
#include "analysis/parsers/vbt_parser.h"
#include "analysis/parsers/vbi_parser.h"
#include "analysis/parsers/vbs4_parser.h"
#include "test_analysis_data.h"

#include <filesystem>
#include <fstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

void set_fourcc(char dst[4], const char (&src)[5]) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

template <typename T>
void write_struct(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

#ifdef _WIN32
std::vector<uint8_t> compress_xpress_huff_for_test(const std::vector<uint8_t>& input) {
    using CompressorHandle = void*;
    using CreateCompressorProc = BOOL(WINAPI*)(DWORD, void*, CompressorHandle*);
    using CompressProc = BOOL(WINAPI*)(CompressorHandle, void*, size_t, void*, size_t, size_t*);
    using CloseCompressorProc = BOOL(WINAPI*)(CompressorHandle);
    constexpr DWORD kXpressHuff = 4;

    HMODULE cabinet = LoadLibraryA("Cabinet.dll");
    REQUIRE(cabinet != nullptr);
    auto create_compressor = reinterpret_cast<CreateCompressorProc>(
        GetProcAddress(cabinet, "CreateCompressor"));
    auto compress = reinterpret_cast<CompressProc>(
        GetProcAddress(cabinet, "Compress"));
    auto close_compressor = reinterpret_cast<CloseCompressorProc>(
        GetProcAddress(cabinet, "CloseCompressor"));
    REQUIRE(create_compressor != nullptr);
    REQUIRE(compress != nullptr);
    REQUIRE(close_compressor != nullptr);

    CompressorHandle compressor = nullptr;
    REQUIRE(create_compressor(kXpressHuff, nullptr, &compressor));

    std::vector<uint8_t> output(input.size() + 4096);
    size_t output_size = 0;
    REQUIRE(compress(compressor,
                     const_cast<uint8_t*>(input.data()),
                     input.size(),
                     output.data(),
                     output.size(),
                     &output_size));
    REQUIRE(close_compressor(compressor));
    output.resize(output_size);
    return output;
}
#endif

} // namespace

// ===========================================================================
// VAC1 Container Tests
// ===========================================================================

TEST_CASE("VAC1: open and embedded sections", "[analysis][vac]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::AnalysisContainerFile vac;
    REQUIRE(vac.open(data.vac_path()));
    REQUIRE(vac.header().magic[0] == 'V');
    REQUIRE(vac.header().magic[1] == 'A');
    REQUIRE(vac.header().magic[2] == 'C');
    REQUIRE(vac.header().magic[3] == '1');
    REQUIRE(vac.section("VBS4") != nullptr);
    REQUIRE(vac.section("VBI2") != nullptr);
    REQUIRE(vac.section("VBT1") != nullptr);

    vr::analysis::VbtFile vbt;
    const auto* vbt_section = vac.section("VBT1");
    REQUIRE(vbt.open_region(vac.path(), vbt_section->offset, vbt_section->size));
    REQUIRE(vbt.packet_count() > 0);

    vr::analysis::VbiFile vbi;
    const auto* vbi_section = vac.section("VBI2");
    REQUIRE(vbi.open_region(vac.path(), vbi_section->offset, vbi_section->size));
    REQUIRE(vbi.nalu_count() > 0);

    vr::analysis::Vbs4File vbs4;
    const auto* vbs4_section = vac.section("VBS4");
    REQUIRE(vbs4.open_region(vac.path(), vbs4_section->offset, vbs4_section->size));
    REQUIRE(vbs4.frame_count() > 0);
}

// ===========================================================================
// VBT Tests
// ===========================================================================

TEST_CASE("VBT: open and header", "[analysis][vbt]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(data.vbt_path()));

    auto& h = vbt.header();
    REQUIRE(h.magic[0] == 'V');
    REQUIRE(h.magic[1] == 'B');
    REQUIRE(h.magic[2] == 'T');
    REQUIRE(h.magic[3] == '1');
    REQUIRE(vbt.packet_count() == 600);
    REQUIRE(h.time_base_num == 1);
    REQUIRE(h.time_base_den == 60);
}

TEST_CASE("VBT: first packet is keyframe with PTS=0", "[analysis][vbt]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(data.vbt_path()));

    auto& e = vbt.entry(0);
    REQUIRE(e.flags & VBT_FLAG_KEYFRAME);
    REQUIRE(e.pts == 0);
    REQUIRE(e.size > 0);
}

TEST_CASE("VBT: all packets have positive size", "[analysis][vbt]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(data.vbt_path()));

    for (int i = 0; i < vbt.packet_count(); i++) {
        REQUIRE(vbt.entry(i).size > 0);
    }
}

TEST_CASE("VBT: keyframe count", "[analysis][vbt]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(data.vbt_path()));

    auto kf = vbt.keyframe_indices();
    REQUIRE(kf.size() == 10);
}

TEST_CASE("VBT: total duration ~10s", "[analysis][vbt]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(data.vbt_path()));

    auto& last = vbt.entry(vbt.packet_count() - 1);
    double last_pts_time = static_cast<double>(last.pts) / vbt.header().time_base_den;
    REQUIRE(last_pts_time >= 9.0);
    REQUIRE(last_pts_time <= 10.5);
}

TEST_CASE("VBT: packet_at_pts binary search", "[analysis][vbt]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(data.vbt_path()));

    // PTS 0 should find packet 0
    REQUIRE(vbt.packet_at_pts(0) == 0);

    // PTS equal to first packet's PTS
    int64_t pts0 = vbt.entry(0).pts;
    REQUIRE(vbt.packet_at_pts(pts0) == 0);
}

TEST_CASE("VBT: packet_at_pts supports non-monotonic PTS order", "[analysis][vbt]") {
    auto path = std::filesystem::temp_directory_path() / "voidplayer_unsorted_pts.vbt";

    VbtHeader header{};
    header.magic[0] = 'V';
    header.magic[1] = 'B';
    header.magic[2] = 'T';
    header.magic[3] = '1';
    header.num_packets = 4;
    header.time_base_num = 1;
    header.time_base_den = 1000;

    VbtEntry entries[4]{};
    entries[0].pts = 0;
    entries[0].dts = 0;
    entries[0].poc = 0;
    entries[0].size = 100;
    entries[1].pts = 3000;
    entries[1].dts = 1000;
    entries[1].poc = 1;
    entries[1].size = 100;
    entries[2].pts = 1000;
    entries[2].dts = 2000;
    entries[2].poc = 2;
    entries[2].size = 100;
    entries[3].pts = 2000;
    entries[3].dts = 3000;
    entries[3].poc = 3;
    entries[3].size = 100;

    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(entries), sizeof(entries));
        REQUIRE(out.good());
    }

    vr::analysis::VbtFile vbt;
    REQUIRE(vbt.open(path.string()));

    REQUIRE(vbt.packet_at_pts(0) == 0);
    REQUIRE(vbt.packet_at_pts(1500) == 2);
    REQUIRE(vbt.packet_at_pts(2500) == 3);
    REQUIRE(vbt.packet_at_pts(3000) == 1);

    std::filesystem::remove(path);
}

// ===========================================================================
// VBI Tests
// ===========================================================================

TEST_CASE("VBI: open and header", "[analysis][vbi]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbiFile vbi;
    REQUIRE(vbi.open(data.vbi_path()));

    auto& h = vbi.header();
    REQUIRE(h.magic[0] == 'V');
    REQUIRE(h.magic[1] == 'B');
    REQUIRE(h.magic[2] == 'I');
    REQUIRE(h.magic[3] == '2');
    REQUIRE(h.version == 2);
    REQUIRE(vbi.codec() == VbiCodec::VVC);
    REQUIRE(vbi.unit_kind() == VbiUnitKind::Nalu);
    REQUIRE(vbi.nalu_count() >= 600);
    REQUIRE(h.source_size > 0);
}

TEST_CASE("VBI: offsets strictly increasing and sizes positive", "[analysis][vbi]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbiFile vbi;
    REQUIRE(vbi.open(data.vbi_path()));

    for (int i = 0; i < vbi.nalu_count(); i++) {
        REQUIRE(vbi.entry(i).size > 0);
        if (i > 0) {
            REQUIRE(vbi.entry(i).offset > vbi.entry(i - 1).offset);
        }
    }
}

TEST_CASE("VBI: first NALU is valid type", "[analysis][vbi]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbiFile vbi;
    REQUIRE(vbi.open(data.vbi_path()));

    // First NALU from container: may be AUD(20), VPS(14), SPS(15), PPS(16), or slice
    // depending on how the container stores parameter sets (in extradata vs inline)
    uint8_t nal_type = vbi.entry(0).nal_type;
    REQUIRE(nal_type <= 31); // valid VVC NALU type range
}

TEST_CASE("VBI: VCL and keyframe counts", "[analysis][vbi]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::VbiFile vbi;
    REQUIRE(vbi.open(data.vbi_path()));

    auto vcl = vbi.find_vcl_nalus();
    auto kf = vbi.find_keyframes();
    REQUIRE(vcl.size() == 600);
    REQUIRE(kf.size() >= 1);
}

// ===========================================================================
// ===========================================================================
// VBS4 Tests
// ===========================================================================

TEST_CASE("VBS4: open and header", "[analysis][vbs4]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(data.vbs4_path()));

    auto& h = vbs4.header();
    REQUIRE(h.magic[0] == 'V');
    REQUIRE(h.magic[1] == 'B');
    REQUIRE(h.magic[2] == 'S');
    REQUIRE(h.magic[3] == '4');
    REQUIRE(h.version_major == 4);
    REQUIRE(h.width == 1920);
    REQUIRE(h.height == 1080);
    REQUIRE(vbs4.frame_count() >= 100);
    REQUIRE(vbs4.section("FSUM") != nullptr);
    REQUIRE(vbs4.section("FIDX") != nullptr);
    REQUIRE(vbs4.section("BIDX") != nullptr);
    REQUIRE(vbs4.section("CPAY") != nullptr);
}

TEST_CASE("VBS4: failed reopen clears previous header", "[analysis][vbs4]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(data.vbs4_path()));
    REQUIRE(vbs4.header().width == 1920);

    const auto missing_path =
        std::filesystem::temp_directory_path() / "voidplayer_missing_reopen.vbs4";
    std::filesystem::remove(missing_path);
    REQUIRE(vbs4.open(missing_path.string()) == false);
    REQUIRE(vbs4.frame_count() == 0);
    REQUIRE(vbs4.header().width == 0);
    REQUIRE(vbs4.header().height == 0);
}

TEST_CASE("VBS4: first frame is I-slice (IDR)", "[analysis][vbs4]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(data.vbs4_path()));

    auto fh = vbs4.read_frame_summary(0);
    REQUIRE(fh.slice_type == 2);
    REQUIRE(fh.num_ref_l0 == 0);
    REQUIRE(fh.num_ref_l1 == 0);
}

TEST_CASE("VBS4: read full frame with CU records", "[analysis][vbs4]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(data.vbs4_path()));

    auto frame = vbs4.read_frame(0);
    REQUIRE(frame.summary.poc >= 0);
    REQUIRE(frame.cus.size() > 0);
    REQUIRE(static_cast<int>(frame.cus.size()) == static_cast<int>(frame.summary.num_cus));

    for (const auto& cu : frame.cus) {
        REQUIRE(cu.common.pred_mode <= 3);
        REQUIRE(cu.common.qp <= 63);
    }
}

TEST_CASE("VBS4: inter frames have references", "[analysis][vbs4]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(data.vbs4_path()));

    int inter_with_refs = 0;
    for (int i = 1; i < std::min(50, vbs4.frame_count()); i++) {
        auto fh = vbs4.read_frame_summary(i);
        if (fh.slice_type != 2 && (fh.num_ref_l0 > 0 || fh.num_ref_l1 > 0)) {
            inter_with_refs++;
        }
    }
    REQUIRE(inter_with_refs > 0);
}

TEST_CASE("VBS4: avg QP in valid range", "[analysis][vbs4]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(data.vbs4_path()));

    for (int i = 0; i < std::min(20, vbs4.frame_count()); i++) {
        auto fh = vbs4.read_frame_summary(i);
        REQUIRE(fh.avg_qp <= 63);
    }
}

TEST_CASE("VBS4: read_all_frame_summaries", "[analysis][vbs4]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(data.vbs4_path()));

    auto summaries = vbs4.read_all_frame_summaries();
    REQUIRE(static_cast<int>(summaries.size()) == vbs4.frame_count());

    for (const auto& fh : summaries) {
        REQUIRE(fh.temporal_id <= 6);
    }
}

TEST_CASE("VBS4: temporal ID range", "[analysis][vbs4]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(data.vbs4_path()));

    for (int i = 0; i < std::min(30, vbs4.frame_count()); i++) {
        auto fh = vbs4.read_frame_summary(i);
        REQUIRE(fh.temporal_id <= 6);
    }
}
