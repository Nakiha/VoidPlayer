#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "analysis/parsers/analysis_container.h"
#include "analysis/parsers/vbt_parser.h"
#include "analysis/parsers/vbi_parser.h"
#include "analysis/parsers/vbs3_parser.h"
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
    REQUIRE(vac.section("VBS3") != nullptr);
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

    vr::analysis::Vbs3File vbs3;
    const auto* vbs3_section = vac.section("VBS3");
    REQUIRE(vbs3.open_region(vac.path(), vbs3_section->offset, vbs3_section->size));
    REQUIRE(vbs3.frame_count() > 0);
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
// VBS3 Tests
// ===========================================================================

TEST_CASE("VBS3: open and header", "[analysis][vbs3]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs3File vbs3;
    REQUIRE(vbs3.open(data.vbs3_path()));

    auto& h = vbs3.header();
    REQUIRE(h.magic[0] == 'V');
    REQUIRE(h.magic[1] == 'B');
    REQUIRE(h.magic[2] == 'S');
    REQUIRE(h.magic[3] == '3');
    REQUIRE(h.version_major == 3);
    REQUIRE(h.width == 1920);
    REQUIRE(h.height == 1080);
    REQUIRE(vbs3.frame_count() >= 100);
}

TEST_CASE("VBS3: failed reopen clears previous header", "[analysis][vbs3]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs3File vbs3;
    REQUIRE(vbs3.open(data.vbs3_path()));
    REQUIRE(vbs3.header().width == 1920);

    const auto missing_path =
        std::filesystem::temp_directory_path() / "voidplayer_missing_reopen.vbs3";
    std::filesystem::remove(missing_path);
    REQUIRE(vbs3.open(missing_path.string()) == false);
    REQUIRE(vbs3.frame_count() == 0);
    REQUIRE(vbs3.header().width == 0);
    REQUIRE(vbs3.header().height == 0);
}

TEST_CASE("VBS3: first frame is I-slice (IDR)", "[analysis][vbs3]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs3File vbs3;
    REQUIRE(vbs3.open(data.vbs3_path()));

    auto fh = vbs3.read_frame_summary(0);
    REQUIRE(fh.slice_type == 2); // I-slice
    REQUIRE(fh.num_ref_l0 == 0);
    REQUIRE(fh.num_ref_l1 == 0);
}

TEST_CASE("VBS3: read full frame with CU records", "[analysis][vbs3]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs3File vbs3;
    REQUIRE(vbs3.open(data.vbs3_path()));

    auto frame = vbs3.read_frame(0);
    REQUIRE(frame.summary.poc >= 0);
    REQUIRE(frame.cus.size() > 0);
    REQUIRE(static_cast<int>(frame.cus.size()) == static_cast<int>(frame.summary.num_cus));

    // All CUs should have valid pred_mode
    for (const auto& cu : frame.cus) {
        REQUIRE(cu.common.pred_mode <= 3);
        REQUIRE(cu.common.qp <= 63);
    }
}

#ifdef _WIN32
TEST_CASE("VBS3: read XPRESS Huffman compressed frame payload", "[analysis][vbs3]") {
    std::vector<uint8_t> raw_payload;
    auto append = [&raw_payload](const auto& value) {
        const auto* first = reinterpret_cast<const uint8_t*>(&value);
        raw_payload.insert(raw_payload.end(), first, first + sizeof(value));
    };

    VbsCuCommon intra_common{};
    intra_common.x = 0;
    intra_common.y = 0;
    intra_common.w = 16;
    intra_common.h = 16;
    intra_common.qp = 22;
    intra_common.pred_mode = 1;
    VbsCuIntra intra{};
    intra.intra_mode = 10;
    append(intra_common);
    append(intra);

    VbsCuCommon inter_common{};
    inter_common.x = 16;
    inter_common.y = 0;
    inter_common.w = 16;
    inter_common.h = 16;
    inter_common.qp = 24;
    inter_common.pred_mode = 0;
    VbsCuInter inter{};
    inter.merge_flag = 1;
    inter.inter_dir = 1;
    inter.mv_l0_x = 4;
    inter.mv_l0_y = -2;
    inter.ref_l0 = 0;
    inter.ref_l1 = -1;
    append(inter_common);
    append(inter);

    const auto compressed_payload = compress_xpress_huff_for_test(raw_payload);
    REQUIRE(!compressed_payload.empty());

    const auto path = std::filesystem::temp_directory_path() / "voidplayer_compressed_frame.vbs3";
    const uint64_t cubl_offset = sizeof(Vbs3Header);
    const uint64_t cubl_size = compressed_payload.size();
    const uint64_t fsum_offset = cubl_offset + cubl_size;
    const uint64_t fsum_size = sizeof(Vbs3FrameSummary);
    const uint64_t cuid_offset = fsum_offset + fsum_size;
    const uint64_t cuid_size = sizeof(Vbs3CuIndexEntry);
    const uint64_t section_table_offset = cuid_offset + cuid_size;
    const uint64_t file_size = section_table_offset + 3 * sizeof(Vbs3SectionEntry);

    Vbs3Header header{};
    set_fourcc(header.magic, "VBS3");
    header.version_major = 3;
    header.version_minor = 1;
    header.header_size = sizeof(Vbs3Header);
    header.section_entry_size = sizeof(Vbs3SectionEntry);
    header.width = 32;
    header.height = 16;
    header.frame_count = 1;
    header.section_count = 3;
    header.section_table_offset = section_table_offset;
    header.file_size = file_size;

    Vbs3FrameSummary summary{};
    summary.coded_order = 0;
    summary.vcl_nalu_index = 0xFFFFFFFFu;
    summary.slice_type = 2;
    summary.avg_qp = 23;
    summary.qp_min = 22;
    summary.qp_max = 24;
    summary.num_cus = 2;
    summary.cu_index_entry = 0;

    Vbs3CuIndexEntry index{};
    index.byte_size = compressed_payload.size();
    index.cu_count = 2;
    index.flags = VBS3_CUID_FLAG_COMPRESSED_XPRESS_HUFF;

    Vbs3SectionEntry sections[3]{};
    set_fourcc(sections[0].type, "FSUM");
    sections[0].offset = fsum_offset;
    sections[0].size = fsum_size;
    sections[0].entry_size = sizeof(Vbs3FrameSummary);
    sections[0].entry_count = 1;
    set_fourcc(sections[1].type, "CUID");
    sections[1].offset = cuid_offset;
    sections[1].size = cuid_size;
    sections[1].entry_size = sizeof(Vbs3CuIndexEntry);
    sections[1].entry_count = 1;
    set_fourcc(sections[2].type, "CUBL");
    sections[2].flags = VBS3_CUBL_SECTION_FLAG_PER_FRAME_COMPRESSION;
    sections[2].offset = cubl_offset;
    sections[2].size = cubl_size;
    sections[2].entry_count = 1;

    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out);
        write_struct(out, header);
        out.write(reinterpret_cast<const char*>(compressed_payload.data()),
                  static_cast<std::streamsize>(compressed_payload.size()));
        write_struct(out, summary);
        write_struct(out, index);
        out.write(reinterpret_cast<const char*>(sections), sizeof(sections));
        REQUIRE(out.good());
    }

    vr::analysis::Vbs3File vbs3;
    REQUIRE(vbs3.open(path.string()));
    const auto frame = vbs3.read_frame(0);
    REQUIRE(frame.cus.size() == 2);
    REQUIRE(frame.cus[0].common.pred_mode == 1);
    REQUIRE(frame.cus[0].common.qp == 22);
    REQUIRE(frame.cus[0].intra.intra_mode == 10);
    REQUIRE(frame.cus[1].common.pred_mode == 0);
    REQUIRE(frame.cus[1].common.qp == 24);
    REQUIRE(frame.cus[1].inter.mv_l0_x == 4);
    REQUIRE(frame.cus[1].inter.mv_l0_y == -2);

    vbs3.close();
    std::filesystem::remove(path);
}
#endif

TEST_CASE("VBS3: inter frames have references", "[analysis][vbs3]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs3File vbs3;
    REQUIRE(vbs3.open(data.vbs3_path()));

    int inter_with_refs = 0;
    for (int i = 1; i < std::min(50, vbs3.frame_count()); i++) {
        auto fh = vbs3.read_frame_summary(i);
        if (fh.slice_type != 2) { // not I-slice
            if (fh.num_ref_l0 > 0 || fh.num_ref_l1 > 0) {
                inter_with_refs++;
            }
        }
    }
    REQUIRE(inter_with_refs > 0);
}

TEST_CASE("VBS3: avg QP in valid range", "[analysis][vbs3]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs3File vbs3;
    REQUIRE(vbs3.open(data.vbs3_path()));

    for (int i = 0; i < std::min(20, vbs3.frame_count()); i++) {
        auto fh = vbs3.read_frame_summary(i);
        REQUIRE(fh.avg_qp <= 63);
    }
}

TEST_CASE("VBS3: read_all_frame_summaries", "[analysis][vbs3]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs3File vbs3;
    REQUIRE(vbs3.open(data.vbs3_path()));

    auto summaries = vbs3.read_all_frame_summaries();
    REQUIRE(static_cast<int>(summaries.size()) == vbs3.frame_count());

    // Verify temporal IDs are in reasonable range
    for (const auto& fh : summaries) {
        REQUIRE(fh.temporal_id <= 6);
    }
}

TEST_CASE("VBS3: temporal ID range", "[analysis][vbs3]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs3File vbs3;
    REQUIRE(vbs3.open(data.vbs3_path()));

    for (int i = 0; i < std::min(30, vbs3.frame_count()); i++) {
        auto fh = vbs3.read_frame_summary(i);
        REQUIRE(fh.temporal_id <= 6);
    }
}
