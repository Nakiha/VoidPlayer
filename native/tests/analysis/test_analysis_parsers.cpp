#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "analysis/cache/vacache_store.h"
#include "analysis/cache/overlay_chunk.h"
#include "analysis/analysis_manager.h"
#include "analysis/parsers/vac2_parser.h"
#include "analysis/parsers/vachunk_parser.h"
#include "analysis/parsers/vbt_parser.h"
#include "analysis/parsers/vbi_parser.h"
#include "analysis/parsers/vbs4_parser.h"
#include "test_analysis_data.h"

#include <filesystem>
#include <fstream>
#include <array>
#include <cstring>
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

std::vector<uint8_t> u32_le(uint32_t value) {
    return {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 24) & 0xFF),
    };
}

void append_bytes(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

void write_minimal_h264_vbs4(const std::filesystem::path& path,
                             uint16_t stream_with_extra_raw_byte) {
    std::array<uint16_t, 13> stream_ids{
        VBS4_STREAM_FRAME_PREFIX,
        VBS4_H264_IS_INTRA,
        VBS4_H264_SKIP_FLAG,
        VBS4_H264_MERGE_FLAG,
        VBS4_H264_INTER_DIR,
        VBS4_H264_QP_DELTA,
        VBS4_H264_INTRA_MODE,
        VBS4_H264_REF_L0,
        VBS4_H264_REF_L1,
        VBS4_H264_MV_L0_X,
        VBS4_H264_MV_L0_Y,
        VBS4_H264_MV_L1_X,
        VBS4_H264_MV_L1_Y,
    };

    Vbs4DecodedBlockHeader block_header{};
    set_fourcc(block_header.magic, "BLK4");
    block_header.header_size = sizeof(Vbs4DecodedBlockHeader);
    block_header.stream_entry_size = sizeof(Vbs4StreamEntry);
    block_header.stream_count = static_cast<uint16_t>(stream_ids.size());
    block_header.frame_count = 1;
    block_header.record_count = 1;

    std::vector<Vbs4StreamEntry> stream_entries;
    std::vector<uint8_t> decoded(sizeof(Vbs4DecodedBlockHeader) +
                                 stream_ids.size() * sizeof(Vbs4StreamEntry));
    std::memcpy(decoded.data(), &block_header, sizeof(block_header));

    for (uint16_t id : stream_ids) {
        std::vector<uint8_t> bytes;
        uint16_t encoding = VBS4_ENCODING_RAW;
        uint32_t value_count = 1;
        if (id == VBS4_STREAM_FRAME_PREFIX) {
            encoding = VBS4_ENCODING_FRAME_PREFIX_U32;
            value_count = 2;
            append_bytes(bytes, u32_le(0));
            append_bytes(bytes, u32_le(1));
        } else {
            bytes.push_back(id == VBS4_H264_IS_INTRA ? 1 : 0);
            if (id == stream_with_extra_raw_byte) {
                bytes.push_back(0xEE);
            }
        }

        Vbs4StreamEntry entry{};
        entry.stream_id = id;
        entry.encoding = encoding;
        entry.offset = static_cast<uint32_t>(decoded.size());
        entry.size = static_cast<uint32_t>(bytes.size());
        entry.value_count = value_count;
        stream_entries.push_back(entry);
        append_bytes(decoded, bytes);
    }
    std::memcpy(decoded.data() + sizeof(Vbs4DecodedBlockHeader),
                stream_entries.data(),
                stream_entries.size() * sizeof(Vbs4StreamEntry));

    const uint64_t section_table_offset = sizeof(Vbs4Header);
    const uint64_t fsum_offset = section_table_offset + 4 * sizeof(Vbs4SectionEntry);
    const uint64_t fidx_offset = fsum_offset + sizeof(Vbs4FrameSummary);
    const uint64_t bidx_offset = fidx_offset + sizeof(Vbs4FrameIndexEntry);
    const uint64_t cpay_offset = bidx_offset + sizeof(Vbs4BlockIndexEntry);
    const uint64_t file_size = cpay_offset + decoded.size();

    Vbs4Header header{};
    set_fourcc(header.magic, "VBS4");
    header.version_major = 4;
    header.header_size = sizeof(Vbs4Header);
    header.section_entry_size = sizeof(Vbs4SectionEntry);
    header.codec = static_cast<uint16_t>(VbiCodec::H264);
    header.width = 16;
    header.height = 16;
    header.frame_count = 1;
    header.block_count = 1;
    header.section_count = 4;
    header.section_table_offset = section_table_offset;
    header.file_size = file_size;

    Vbs4SectionEntry fsum{};
    set_fourcc(fsum.type, "FSUM");
    fsum.offset = fsum_offset;
    fsum.size = sizeof(Vbs4FrameSummary);
    fsum.entry_size = sizeof(Vbs4FrameSummary);
    fsum.entry_count = 1;

    Vbs4SectionEntry fidx{};
    set_fourcc(fidx.type, "FIDX");
    fidx.offset = fidx_offset;
    fidx.size = sizeof(Vbs4FrameIndexEntry);
    fidx.entry_size = sizeof(Vbs4FrameIndexEntry);
    fidx.entry_count = 1;

    Vbs4SectionEntry bidx{};
    set_fourcc(bidx.type, "BIDX");
    bidx.offset = bidx_offset;
    bidx.size = sizeof(Vbs4BlockIndexEntry);
    bidx.entry_size = sizeof(Vbs4BlockIndexEntry);
    bidx.entry_count = 1;

    Vbs4SectionEntry cpay{};
    set_fourcc(cpay.type, "CPAY");
    cpay.offset = cpay_offset;
    cpay.size = decoded.size();
    cpay.entry_count = 1;

    Vbs4FrameSummary summary{};
    summary.slice_type = 2;
    summary.avg_qp = 22;
    summary.qp_min = 22;
    summary.qp_max = 22;
    summary.num_cus = 1;
    summary.cu_index_entry = 0;

    Vbs4FrameIndexEntry frame_index{};
    frame_index.block_index = 0;
    frame_index.record_count = 1;

    Vbs4BlockIndexEntry block_index{};
    block_index.frame_count = 1;
    block_index.record_count = 1;
    block_index.payload_size = decoded.size();
    block_index.decoded_size = decoded.size();
    block_index.compression = VBS4_COMPRESSION_NONE;

    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    write_struct(out, header);
    write_struct(out, fsum);
    write_struct(out, fidx);
    write_struct(out, bidx);
    write_struct(out, cpay);
    write_struct(out, summary);
    write_struct(out, frame_index);
    write_struct(out, block_index);
    out.write(reinterpret_cast<const char*>(decoded.data()),
              static_cast<std::streamsize>(decoded.size()));
    REQUIRE(out.good());
}

} // namespace

// ===========================================================================
// VAC2 Base Container Tests
// ===========================================================================

TEST_CASE("VAC2: write and read base index sections", "[analysis][vac2]") {
    const auto path = std::filesystem::temp_directory_path() / "voidplayer_test_base.vac";

    vr::analysis::Vac2BaseData data;
    data.codec = VbiCodec::VVC;
    data.track_index = 0;
    data.time_base_num = 1;
    data.time_base_den = 60;
    data.width = 1920;
    data.height = 1080;
    data.source_size = 123456;
    data.source_mtime_unix_ms = 987654321;
    data.content_revision = 42;
    data.metadata_json = R"({"schema":"vac2-test"})";

    Vac2PacketEntry packet0{};
    packet0.pts = 0;
    packet0.dts = 0;
    packet0.duration = 1;
    packet0.size = 100;
    packet0.flags = VAC2_PACKET_FLAG_KEYFRAME;
    packet0.file_offset = 1000;
    packet0.format_offset = 1000;
    packet0.first_unit = 0;
    packet0.unit_count = 2;
    packet0.au_index = 0;

    Vac2PacketEntry packet1{};
    packet1.pts = 1;
    packet1.dts = 1;
    packet1.duration = 1;
    packet1.size = 80;
    packet1.file_offset = 1100;
    packet1.format_offset = 1100;
    packet1.first_unit = 2;
    packet1.unit_count = 1;
    packet1.au_index = 1;
    data.packets = {packet0, packet1};

    Vac2BitstreamUnitEntry unit0{};
    unit0.packet_index = 0;
    unit0.au_index = 0;
    unit0.offset = 0;
    unit0.size = 32;
    unit0.nal_type = 14;
    unit0.unit_kind = static_cast<uint8_t>(VbiUnitKind::Nalu);
    unit0.flags = VAC2_UNIT_FLAG_PARAMETER_SET;
    unit0.pset_snapshot = 0;

    Vac2BitstreamUnitEntry unit1{};
    unit1.packet_index = 0;
    unit1.au_index = 0;
    unit1.offset = 32;
    unit1.size = 68;
    unit1.nal_type = 7;
    unit1.temporal_id = 0;
    unit1.unit_kind = static_cast<uint8_t>(VbiUnitKind::Nalu);
    unit1.flags = VAC2_UNIT_FLAG_IS_VCL |
                  VAC2_UNIT_FLAG_IS_SLICE |
                  VAC2_UNIT_FLAG_IS_KEYFRAME;
    unit1.pset_snapshot = 0;

    Vac2BitstreamUnitEntry unit2{};
    unit2.packet_index = 1;
    unit2.au_index = 1;
    unit2.offset = 100;
    unit2.size = 80;
    unit2.nal_type = 1;
    unit2.temporal_id = 1;
    unit2.unit_kind = static_cast<uint8_t>(VbiUnitKind::Nalu);
    unit2.flags = VAC2_UNIT_FLAG_IS_VCL | VAC2_UNIT_FLAG_IS_SLICE;
    unit2.pset_snapshot = 0;
    data.units = {unit0, unit1, unit2};

    Vac2FrameEntry frame0{};
    frame0.first_packet = 0;
    frame0.packet_count = 1;
    frame0.first_unit = 0;
    frame0.unit_count = 2;
    frame0.pts = 0;
    frame0.dts = 0;
    frame0.duration = 1;
    frame0.coded_order = 0;
    frame0.display_order = 0;
    frame0.poc = 0;
    frame0.frame_size = 100;
    frame0.rap_distance = 0;
    frame0.flags = VAC2_FRAME_FLAG_KEYFRAME | VAC2_FRAME_FLAG_RAP;

    Vac2FrameEntry frame1{};
    frame1.first_packet = 1;
    frame1.packet_count = 1;
    frame1.first_unit = 2;
    frame1.unit_count = 1;
    frame1.pts = 1;
    frame1.dts = 1;
    frame1.duration = 1;
    frame1.coded_order = 1;
    frame1.display_order = 1;
    frame1.poc = 2;
    frame1.frame_size = 80;
    frame1.rap_distance = 1;
    data.frames = {frame0, frame1};

    Vac2FrameSummaryEntry summary0{};
    summary0.poc = 0;
    summary0.coded_order = 0;
    summary0.first_vcl_unit = 1;
    summary0.flags = VAC2_FRAME_SUMMARY_FLAG_EXACT_REFS;
    summary0.slice_type = 2;
    summary0.nal_type = 7;
    summary0.qp_kind = VAC2_QP_KIND_SLICE;
    summary0.qp_avg = 26;
    summary0.qp_min = 26;
    summary0.qp_max = 26;

    Vac2FrameSummaryEntry summary1{};
    summary1.poc = 2;
    summary1.coded_order = 1;
    summary1.first_vcl_unit = 2;
    summary1.temporal_id = 1;
    summary1.slice_type = 1;
    summary1.nal_type = 1;
    summary1.qp_kind = VAC2_QP_KIND_SLICE;
    summary1.qp_avg = 28;
    summary1.qp_min = 28;
    summary1.qp_max = 28;
    summary1.num_ref_l0 = 1;
    summary1.ref_pocs_l0[0] = 0;
    data.frame_summaries = {summary0, summary1};

    REQUIRE(vr::analysis::write_vac2_base_container(path.string(), data));

    vr::analysis::Vac2BaseFile vac2;
    REQUIRE(vac2.open(path.string()));
    REQUIRE(vac2.header().magic[0] == 'V');
    REQUIRE(vac2.header().magic[1] == 'A');
    REQUIRE(vac2.header().magic[2] == 'C');
    REQUIRE(vac2.header().magic[3] == '2');
    REQUIRE(vac2.header().codec == static_cast<uint16_t>(VbiCodec::VVC));
    REQUIRE(vac2.header().packet_count == 2);
    REQUIRE(vac2.header().unit_count == 3);
    REQUIRE(vac2.header().au_count == 2);
    REQUIRE(vac2.section("META") != nullptr);
    REQUIRE(vac2.section("PKT2") != nullptr);
    REQUIRE(vac2.section("BSU2") != nullptr);
    REQUIRE(vac2.section("AUF2") != nullptr);
    REQUIRE(vac2.section("FSUM") != nullptr);

    REQUIRE(vac2.metadata_json() == data.metadata_json);
    REQUIRE(vac2.packets().size() == 2);
    REQUIRE(vac2.units().size() == 3);
    REQUIRE(vac2.frames().size() == 2);
    REQUIRE(vac2.frame_summaries().size() == 2);
    REQUIRE(vac2.packets()[0].flags & VAC2_PACKET_FLAG_KEYFRAME);
    REQUIRE(vac2.units()[1].flags & VAC2_UNIT_FLAG_IS_KEYFRAME);
    REQUIRE(vac2.frames()[1].poc == 2);
    REQUIRE(vac2.frame_summaries()[1].num_ref_l0 == 1);
    REQUIRE(vac2.frame_summaries()[1].ref_pocs_l0[0] == 0);
    REQUIRE(vac2.frame_summaries()[1].qp_kind == VAC2_QP_KIND_SLICE);

    std::filesystem::remove(path);
}

TEST_CASE("VAC2: writer respects output budget", "[analysis][vac2]") {
    const auto path = std::filesystem::temp_directory_path() /
        "voidplayer_test_base_budget.vac";
    vr::analysis::Vac2BaseData data;
    data.metadata_json = "{}";

    Vac2PacketEntry packet{};
    data.packets = {packet};
    Vac2BitstreamUnitEntry unit{};
    data.units = {unit};
    Vac2FrameEntry frame{};
    data.frames = {frame};
    Vac2FrameSummaryEntry summary{};
    data.frame_summaries = {summary};

    REQUIRE_FALSE(vr::analysis::write_vac2_base_container(path.string(), data, 16));
    std::filesystem::remove(path);
}

// ===========================================================================
// VACHUNK Derived Chunk Tests
// ===========================================================================

TEST_CASE("VACHUNK: write and read exact frame summary chunk", "[analysis][vachunk]") {
    const auto path = std::filesystem::temp_directory_path() /
        "voidplayer_test_summary.vck";

    Vac2FrameSummaryEntry summary0{};
    summary0.poc = 4;
    summary0.coded_order = 2;
    summary0.first_vcl_unit = 10;
    summary0.flags = VAC2_FRAME_SUMMARY_FLAG_EXACT_QP |
                     VAC2_FRAME_SUMMARY_FLAG_EXACT_REFS;
    summary0.temporal_id = 1;
    summary0.slice_type = 1;
    summary0.nal_type = 1;
    summary0.qp_kind = VAC2_QP_KIND_EXACT;
    summary0.qp_avg = 31;
    summary0.qp_min = 28;
    summary0.qp_max = 34;
    summary0.num_ref_l0 = 1;
    summary0.ref_pocs_l0[0] = 0;

    Vac2FrameSummaryEntry summary1 = summary0;
    summary1.poc = 6;
    summary1.coded_order = 3;
    summary1.first_vcl_unit = 11;
    summary1.qp_avg = 29;

    std::vector<Vac2FrameSummaryEntry> summaries{summary0, summary1};

    vr::analysis::VachunkData data;
    data.kind = VachunkKind::FrameSummaryExact;
    data.codec = VbiCodec::VVC;
    data.feature_flags = VACHUNK_FEATURE_QP | VACHUNK_FEATURE_REF_INDEXES;
    data.base_content_revision = 42;
    data.generator_revision = 7;
    data.start_frame = 2;
    data.end_frame = 3;
    data.start_packet = 2;
    data.end_packet = 3;
    data.start_unit = 10;
    data.end_unit = 11;
    data.sections.push_back(vr::analysis::make_vachunk_string_section(
        "META", R"({"schema":"vachunk-test"})"));
    data.sections.push_back(vr::analysis::make_vachunk_record_section(
        "FSUM", summaries));

    REQUIRE(vr::analysis::write_vachunk_file(path.string(), data));

    vr::analysis::VachunkFile chunk;
    REQUIRE(chunk.open(path.string()));
    REQUIRE(chunk.header().magic[0] == 'V');
    REQUIRE(chunk.header().magic[1] == 'C');
    REQUIRE(chunk.header().magic[2] == 'K');
    REQUIRE(chunk.header().magic[3] == '1');
    REQUIRE(chunk.header().kind == static_cast<uint16_t>(VachunkKind::FrameSummaryExact));
    REQUIRE(chunk.header().codec == static_cast<uint16_t>(VbiCodec::VVC));
    REQUIRE(chunk.header().feature_flags & VACHUNK_FEATURE_QP);
    REQUIRE(chunk.header().base_content_revision == 42);
    REQUIRE(chunk.header().start_frame == 2);
    REQUIRE(chunk.header().end_frame == 3);
    REQUIRE(chunk.section("META") != nullptr);
    REQUIRE(chunk.section("FSUM") != nullptr);
    REQUIRE(chunk.section("FSUM")->entry_size == sizeof(Vac2FrameSummaryEntry));
    REQUIRE(chunk.section("FSUM")->entry_count == 2);

    std::vector<uint8_t> raw;
    REQUIRE(chunk.read_section("FSUM", raw));
    REQUIRE(raw.size() == summaries.size() * sizeof(Vac2FrameSummaryEntry));
    const auto* decoded = reinterpret_cast<const Vac2FrameSummaryEntry*>(raw.data());
    REQUIRE(decoded[0].qp_kind == VAC2_QP_KIND_EXACT);
    REQUIRE(decoded[0].qp_avg == 31);
    REQUIRE(decoded[1].poc == 6);

    std::filesystem::remove(path);
}

TEST_CASE("VACHUNK: writer rejects invalid range and tiny budget", "[analysis][vachunk]") {
    const auto path = std::filesystem::temp_directory_path() /
        "voidplayer_test_invalid.vck";

    vr::analysis::VachunkData data;
    data.kind = VachunkKind::Overlay;
    data.codec = VbiCodec::HEVC;
    data.start_frame = 10;
    data.end_frame = 9;
    data.sections.push_back(vr::analysis::make_vachunk_string_section("META", "{}"));

    REQUIRE_FALSE(vr::analysis::write_vachunk_file(path.string(), data));

    data.start_frame = 10;
    data.end_frame = 10;
    REQUIRE_FALSE(vr::analysis::write_vachunk_file(path.string(), data, 8));
    std::filesystem::remove(path);
}

TEST_CASE("VACHUNK: overlay chunk carries VBS4 frame data",
          "[analysis][vachunk][overlay]") {
    namespace fs = std::filesystem;
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(data.vbs4_path()));
    REQUIRE(vbs4.frame_count() >= 2);

    vr::analysis::VachunkData chunk_data;
    REQUIRE(vr::analysis::build_overlay_vachunk_from_vbs4(
        vbs4, 0, 1, chunk_data));
    REQUIRE(chunk_data.kind == VachunkKind::Overlay);
    REQUIRE(chunk_data.sections.size() == 3);
    REQUIRE((chunk_data.feature_flags & VACHUNK_FEATURE_CU_GEOMETRY) != 0);
    REQUIRE((chunk_data.feature_flags & VACHUNK_FEATURE_QP) != 0);

    const auto path = fs::temp_directory_path() / "voidplayer_test_overlay.vck";
    fs::remove(path);
    REQUIRE(vr::analysis::write_vachunk_file(path.string(), chunk_data));

    vr::analysis::VachunkFile chunk;
    REQUIRE(chunk.open(path.string()));
    REQUIRE(chunk.header().kind ==
            static_cast<uint16_t>(VachunkKind::Overlay));
    REQUIRE(chunk.header().start_frame == 0);
    REQUIRE(chunk.header().end_frame == 1);
    REQUIRE(chunk.section("FSUM") != nullptr);
    REQUIRE(chunk.section("FIDX") != nullptr);
    REQUIRE(chunk.section("CU4R") != nullptr);

    vr::analysis::VachunkOverlayFrameData frame0;
    REQUIRE(vr::analysis::read_overlay_vachunk_frame(chunk, 0, frame0));
    const auto legacy0 = vbs4.read_frame(0);
    REQUIRE(frame0.summary.avg_qp == legacy0.summary.avg_qp);
    REQUIRE(frame0.summary.num_cus == legacy0.summary.num_cus);
    REQUIRE(frame0.cus.size() == legacy0.cus.size());
    if (!frame0.cus.empty()) {
        REQUIRE(frame0.cus[0].common.x == legacy0.cus[0].common.x);
        REQUIRE(frame0.cus[0].common.y == legacy0.cus[0].common.y);
        REQUIRE(frame0.cus[0].common.qp == legacy0.cus[0].common.qp);
        REQUIRE(frame0.cus[0].common.pred_mode ==
                legacy0.cus[0].common.pred_mode);
    }

    vr::analysis::VachunkOverlayFrameData missing;
    REQUIRE_FALSE(vr::analysis::read_overlay_vachunk_frame(chunk, 2, missing));

    chunk.close();
    fs::remove(path);
}

TEST_CASE("AnalysisManager: reads VAC2 base with overlay chunks",
          "[analysis][manager][vac2][vachunk]") {
    namespace fs = std::filesystem;
    auto& test_data = AnalysisTestData::instance();
    REQUIRE(test_data.ensure());

    vr::analysis::Vbs4File vbs4;
    REQUIRE(vbs4.open(test_data.vbs4_path()));
    REQUIRE(vbs4.frame_count() >= 2);

    const auto root = fs::temp_directory_path() / "voidplayer_manager_vac2";
    const auto hash_dir = root / "hash";
    const auto overlay_dir = hash_dir / "chunks" / "overlay";
    fs::remove_all(root);
    REQUIRE(fs::create_directories(overlay_dir));

    vr::analysis::Vac2BaseData base;
    base.codec = static_cast<VbiCodec>(vbs4.header().codec);
    base.time_base_num = 1;
    base.time_base_den = 1000000;
    base.width = vbs4.header().width;
    base.height = vbs4.header().height;
    base.metadata_json = R"({"schema":"manager-vac2-overlay-test"})";

    for (uint32_t i = 0; i < 2; ++i) {
        Vac2PacketEntry packet{};
        packet.pts = static_cast<int64_t>(i) * 40000;
        packet.dts = packet.pts;
        packet.duration = 40000;
        packet.size = 1000;
        packet.file_offset = UINT64_MAX;
        packet.format_offset = UINT64_MAX;
        packet.first_unit = i;
        packet.unit_count = 1;
        packet.au_index = i;
        base.packets.push_back(packet);

        Vac2BitstreamUnitEntry unit{};
        unit.packet_index = i;
        unit.au_index = i;
        unit.offset = i * 1000;
        unit.size = 1000;
        unit.flags = VAC2_UNIT_FLAG_IS_VCL | VAC2_UNIT_FLAG_IS_SLICE;
        unit.pset_snapshot = UINT16_MAX;
        base.units.push_back(unit);

        Vac2FrameEntry frame{};
        frame.first_packet = i;
        frame.packet_count = 1;
        frame.first_unit = i;
        frame.unit_count = 1;
        frame.pts = packet.pts;
        frame.dts = packet.dts;
        frame.duration = packet.duration;
        frame.coded_order = i;
        frame.display_order = static_cast<int32_t>(i);
        frame.poc = static_cast<int32_t>(i);
        frame.frame_size = packet.size;
        base.frames.push_back(frame);

        Vac2FrameSummaryEntry summary{};
        summary.poc = static_cast<int32_t>(i);
        summary.coded_order = i;
        summary.first_vcl_unit = i;
        summary.qp_kind = VAC2_QP_KIND_UNKNOWN;
        base.frame_summaries.push_back(summary);
    }

    const auto base_path = hash_dir / "base.vac";
    REQUIRE(vr::analysis::write_vac2_base_container(base_path.string(), base));

    vr::analysis::VachunkData chunk_data;
    REQUIRE(vr::analysis::build_overlay_vachunk_from_vbs4(
        vbs4, 0, 1, chunk_data));
    REQUIRE(vr::analysis::write_vachunk_file(
        (overlay_dir / "overlay_00000000_00000001.vck").string(),
        chunk_data));

    vr::analysis::AnalysisManager manager;
    REQUIRE(manager.load(base_path.string()));
    REQUIRE(manager.frame_count() == 2);
    REQUIRE(manager.video_width() == vbs4.header().width);
    REQUIRE(manager.video_height() == vbs4.header().height);
    REQUIRE(manager.current_frame_idx(45000) == 1);

    const auto frame0 = manager.read_overlay_frame(0);
    const auto legacy0 = vbs4.read_frame(0);
    REQUIRE(frame0.summary.avg_qp == legacy0.summary.avg_qp);
    REQUIRE(frame0.cus.size() == legacy0.cus.size());
    if (!frame0.cus.empty()) {
        REQUIRE(frame0.cus[0].common.qp == legacy0.cus[0].common.qp);
    }

    manager.unload();
    fs::remove_all(root);
}

// ===========================================================================
// VACache Store Tests
// ===========================================================================

TEST_CASE("VACache: publishes base and derived chunks atomically", "[analysis][vacache]") {
    const auto root = std::filesystem::temp_directory_path() / "voidplayer_vacache_test";
    std::filesystem::remove_all(root);

    vr::analysis::VacacheStore store(root.string(), "abc123");
    REQUIRE(store.ensure_layout());
    REQUIRE(std::filesystem::exists(root / "abc123" / "tmp"));
    REQUIRE(std::filesystem::exists(root / "abc123" / "chunks"));
    REQUIRE(store.base_path().find("base.vac") != std::string::npos);

    vr::analysis::Vac2BaseData base;
    base.codec = VbiCodec::HEVC;
    base.time_base_num = 1;
    base.time_base_den = 1000;
    base.content_revision = 88;
    base.metadata_json = R"({"schema":"vac2-cache-test"})";

    Vac2PacketEntry packet{};
    packet.pts = 40;
    packet.dts = 40;
    packet.size = 256;
    packet.file_offset = UINT64_MAX;
    packet.format_offset = UINT64_MAX;
    packet.au_index = 0;
    base.packets = {packet};

    Vac2BitstreamUnitEntry unit{};
    unit.packet_index = 0;
    unit.au_index = 0;
    unit.size = 256;
    unit.nal_type = 19;
    unit.unit_kind = static_cast<uint8_t>(VbiUnitKind::Nalu);
    unit.flags = VAC2_UNIT_FLAG_IS_VCL |
                 VAC2_UNIT_FLAG_IS_SLICE |
                 VAC2_UNIT_FLAG_IS_KEYFRAME;
    unit.pset_snapshot = UINT16_MAX;
    base.units = {unit};

    Vac2FrameEntry frame{};
    frame.first_packet = 0;
    frame.packet_count = 1;
    frame.first_unit = 0;
    frame.unit_count = 1;
    frame.pts = 40;
    frame.dts = 40;
    frame.frame_size = 256;
    frame.flags = VAC2_FRAME_FLAG_KEYFRAME | VAC2_FRAME_FLAG_RAP;
    base.frames = {frame};

    Vac2FrameSummaryEntry summary{};
    summary.first_vcl_unit = 0;
    summary.slice_type = 2;
    summary.nal_type = 19;
    summary.qp_kind = VAC2_QP_KIND_UNKNOWN;
    base.frame_summaries = {summary};

    REQUIRE(store.write_base_atomic(base));
    vr::analysis::Vac2BaseFile base_file;
    REQUIRE(store.open_base(base_file));
    REQUIRE(base_file.header().codec == static_cast<uint16_t>(VbiCodec::HEVC));
    REQUIRE(base_file.header().packet_count == 1);

    vr::analysis::VachunkKey key;
    key.kind = VachunkKind::FrameSummaryExact;
    key.codec = VbiCodec::HEVC;
    key.feature_flags = VACHUNK_FEATURE_QP;
    key.base_content_revision = 88;
    key.generator_revision = 3;
    key.start_frame = 0;
    key.end_frame = 0;
    key.start_packet = 0;
    key.end_packet = 0;
    key.start_unit = 0;
    key.end_unit = 0;

    summary.qp_kind = VAC2_QP_KIND_EXACT;
    summary.qp_avg = 27;
    std::vector<Vac2FrameSummaryEntry> exact{summary};

    vr::analysis::VachunkData chunk_data;
    chunk_data.sections.push_back(
        vr::analysis::make_vachunk_string_section("META", "{}"));
    chunk_data.sections.push_back(
        vr::analysis::make_vachunk_record_section("FSUM", exact));

    REQUIRE(store.write_chunk_atomic(key, chunk_data));
    REQUIRE(std::filesystem::exists(root / "abc123" / "chunks" / "frame_summary_exact"));

    vr::analysis::VachunkFile chunk;
    REQUIRE(store.open_chunk(key, chunk));
    REQUIRE(chunk.header().base_content_revision == 88);
    REQUIRE(chunk.header().generator_revision == 3);

    key.generator_revision = 4;
    vr::analysis::VachunkFile wrong_chunk;
    REQUIRE_FALSE(store.open_chunk(key, wrong_chunk));

    std::filesystem::remove_all(root);
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

TEST_CASE("VBS4: raw streams reject trailing payload bytes", "[analysis][vbs4]") {
    const auto base = std::filesystem::temp_directory_path();

    {
        const auto path = base / "voidplayer_vbs4_trailing_u32_raw.vbs4";
        write_minimal_h264_vbs4(path, VBS4_H264_SKIP_FLAG);
        vr::analysis::Vbs4File vbs4;
        REQUIRE(vbs4.open(path.string()));
        auto frame = vbs4.read_frame(0);
        REQUIRE(frame.cus.empty());
        vbs4.close();
        std::filesystem::remove(path);
    }

    {
        const auto path = base / "voidplayer_vbs4_trailing_i32_raw.vbs4";
        write_minimal_h264_vbs4(path, VBS4_H264_QP_DELTA);
        vr::analysis::Vbs4File vbs4;
        REQUIRE(vbs4.open(path.string()));
        auto frame = vbs4.read_frame(0);
        REQUIRE(frame.cus.empty());
        vbs4.close();
        std::filesystem::remove(path);
    }
}
