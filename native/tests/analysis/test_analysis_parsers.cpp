#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "analysis/cache/vacache_store.h"
#include "analysis/cache/overlay_chunk.h"
#include "analysis/cache/overlay_raster.h"
#include "analysis/cache/overlay_text.h"
#include "analysis/analysis_manager.h"
#include "analysis/parsers/vac2_parser.h"
#include "analysis/parsers/vachunk_parser.h"
#include "test_analysis_data.h"

#include <cstring>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

vr::analysis::VachunkData make_overlay_chunk(uint32_t start_frame,
                                             uint32_t end_frame,
                                             AnalysisCodec codec = AnalysisCodec::H264) {
    const uint32_t frame_count = end_frame >= start_frame
        ? end_frame - start_frame + 1
        : 0;

    std::vector<VachunkFrameSummary> summaries;
    std::vector<VachunkOverlayFrameIndexEntry> frame_index;
    std::vector<vr::analysis::VachunkCuRecord> records;
    summaries.reserve(frame_count);
    frame_index.reserve(frame_count);
    records.reserve(frame_count);

    for (uint32_t i = 0; i < frame_count; ++i) {
        VachunkFrameSummary summary{};
        summary.poc = static_cast<int32_t>(i);
        summary.coded_order = i;
        summary.vcl_unit_index = i;
        summary.slice_type = i == 0 ? 2 : 1;
        summary.nal_unit_type = i == 0 ? 5 : 1;
        summary.avg_qp = static_cast<uint8_t>(22 + i);
        summary.qp_min = summary.avg_qp;
        summary.qp_max = summary.avg_qp;
        summary.num_cus = 1;
        summary.cu_index_entry = i;
        summaries.push_back(summary);

        VachunkOverlayFrameIndexEntry index{};
        index.frame_index = start_frame + i;
        index.first_unit = static_cast<uint32_t>(records.size());
        index.unit_count = 1;
        index.flags =
            VACHUNK_OVERLAY_FRAME_FLAG_COMPLETE |
            VACHUNK_OVERLAY_FRAME_FLAG_EXACT;
        frame_index.push_back(index);

        vr::analysis::VachunkCuRecord cu{};
        cu.common.x = static_cast<uint16_t>(i * 16);
        cu.common.y = 0;
        cu.common.w = 16;
        cu.common.h = 16;
        cu.common.qp = summary.avg_qp;
        cu.common.pred_mode = i == 0 ? 1 : 0;
        cu.common.bit_count = 128 + i * 64;
        cu.inter.merge_flag = i == 0 ? 0 : 1;
        records.push_back(cu);
    }

    vr::analysis::VachunkData data;
    data.kind = VachunkKind::Overlay;
    data.codec = codec;
    data.feature_flags =
        VACHUNK_FEATURE_CU_GEOMETRY |
        VACHUNK_FEATURE_QP |
        VACHUNK_FEATURE_PRED_MODE |
        VACHUNK_FEATURE_MOTION_VECTORS |
        VACHUNK_FEATURE_REF_INDEXES |
        VACHUNK_FEATURE_BIT_COST;
    data.start_frame = start_frame;
    data.end_frame = end_frame;
    data.sections.push_back(
        vr::analysis::make_vachunk_record_section("FSUM", summaries));
    data.sections.push_back(
        vr::analysis::make_vachunk_record_section("FIDX", frame_index));
    data.sections.push_back(
        vr::analysis::make_vachunk_record_section("CU4R", records));
    return data;
}

bool set_overlay_frame_qp(vr::analysis::VachunkData& data,
                          uint32_t local_frame,
                          uint8_t qp) {
    bool updated_summary = false;
    bool updated_record = false;
    for (auto& section : data.sections) {
        const std::string type(section.type, section.type + 4);
        if (type == "FSUM") {
            if (section.entry_size != sizeof(VachunkFrameSummary) ||
                local_frame >= section.entry_count) {
                return false;
            }
            const size_t offset =
                static_cast<size_t>(local_frame) * sizeof(VachunkFrameSummary);
            VachunkFrameSummary summary{};
            std::memcpy(&summary, section.bytes.data() + offset, sizeof(summary));
            summary.avg_qp = qp;
            summary.qp_min = qp;
            summary.qp_max = qp;
            std::memcpy(section.bytes.data() + offset, &summary, sizeof(summary));
            updated_summary = true;
        } else if (type == "CU4R") {
            if (section.entry_size != sizeof(vr::analysis::VachunkCuRecord) ||
                local_frame >= section.entry_count) {
                return false;
            }
            const size_t offset =
                static_cast<size_t>(local_frame) *
                sizeof(vr::analysis::VachunkCuRecord);
            vr::analysis::VachunkCuRecord record{};
            std::memcpy(&record, section.bytes.data() + offset, sizeof(record));
            record.common.qp = qp;
            std::memcpy(section.bytes.data() + offset, &record, sizeof(record));
            updated_record = true;
        }
    }
    return updated_summary && updated_record;
}

void overwrite_u16(const std::filesystem::path& path, size_t offset, uint16_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(file);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    REQUIRE(file);
}

void overwrite_u64(const std::filesystem::path& path, size_t offset, uint64_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(file);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    REQUIRE(file);
}

vr::analysis::Vac2BaseData make_vacache_base_data(uint64_t content_revision) {
    vr::analysis::Vac2BaseData base;
    base.codec = AnalysisCodec::HEVC;
    base.time_base_num = 1;
    base.time_base_den = 1000;
    base.content_revision = content_revision;
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
    unit.unit_kind = static_cast<uint8_t>(AnalysisUnitKind::Nalu);
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
    return base;
}

vr::analysis::VachunkData make_frame_summary_chunk_data(uint8_t qp) {
    Vac2FrameSummaryEntry summary{};
    summary.first_vcl_unit = 0;
    summary.slice_type = 2;
    summary.nal_type = 19;
    summary.qp_kind = VAC2_QP_KIND_EXACT;
    summary.qp_avg = qp;
    std::vector<Vac2FrameSummaryEntry> summaries{summary};

    vr::analysis::VachunkData chunk_data;
    chunk_data.sections.push_back(
        vr::analysis::make_vachunk_string_section("META", "{}"));
    chunk_data.sections.push_back(
        vr::analysis::make_vachunk_record_section("FSUM", summaries));
    return chunk_data;
}

} // namespace

TEST_CASE("Overlay text: builds centered ASCII glyph quads", "[analysis][overlay][text]") {
    vr::analysis::VachunkCuRecord cu{};
    cu.common.qp = 27;
    cu.common.pred_mode = 1;
    cu.common.bit_count = 128000;
    REQUIRE(vr::analysis::overlay_cu_label_text(
                cu, vr::analysis::OverlayCuLabelMode::Qp) == "QP 27");
    REQUIRE(vr::analysis::overlay_cu_label_text(
                cu, vr::analysis::OverlayCuLabelMode::BitCost) == "128K");
    REQUIRE(vr::analysis::overlay_cu_label_text(
                cu, vr::analysis::OverlayCuLabelMode::Prediction) == "INTRA");

    cu.common.pred_mode = 0;
    cu.inter.merge_flag = 1;
    REQUIRE(vr::analysis::overlay_cu_label_text(
                cu, vr::analysis::OverlayCuLabelMode::Prediction) == "MERGE");

    vr::analysis::OverlayTextLayout layout;
    layout.surface_width = 1920;
    layout.surface_height = 1080;
    layout.rect_x0 = 100.0f;
    layout.rect_y0 = 200.0f;
    layout.rect_x1 = 260.0f;
    layout.rect_y1 = 280.0f;
    layout.pixel_scale_x = 1.0f;
    layout.pixel_scale_y = 1.0f;
    layout.target_cell_pixels = 2;
    layout.padding_pixels = 2;

    std::vector<vr::analysis::OverlayGlyphQuad> quads;
    REQUIRE(vr::analysis::append_ascii_overlay_glyph_quads(
        quads,
        "QP 27",
        layout,
        vr::analysis::OverlayColor{255, 255, 255, 255},
        vr::analysis::OverlayColor{0, 0, 0, 192}));
    REQUIRE(!quads.empty());
    for (const auto& quad : quads) {
        REQUIRE(quad.x0 >= layout.rect_x0);
        REQUIRE(quad.y0 >= layout.rect_y0);
        REQUIRE(quad.x1 <= layout.rect_x1);
        REQUIRE(quad.y1 <= layout.rect_y1);
        REQUIRE(quad.atlas_x1 > quad.atlas_x0);
        REQUIRE(quad.atlas_y1 > quad.atlas_y0);
    }

    layout.rect_x1 = layout.rect_x0 + 8.0f;
    quads.clear();
    REQUIRE_FALSE(vr::analysis::append_ascii_overlay_glyph_quads(
        quads,
        "QP 27",
        layout,
        vr::analysis::OverlayColor{255, 255, 255, 255},
        vr::analysis::OverlayColor{}));
    REQUIRE(quads.empty());

    std::vector<uint8_t> atlas;
    int width = 0;
    int height = 0;
    REQUIRE(vr::analysis::build_ascii_overlay_glyph_atlas(atlas, width, height));
    REQUIRE(width == 128);
    REQUIRE(height == 48);
    REQUIRE(atlas.size() == static_cast<size_t>(width) * static_cast<size_t>(height));
    REQUIRE(std::any_of(atlas.begin(), atlas.end(), [](uint8_t value) {
        return value != 0;
    }));
}

// ===========================================================================
// VAC2 Base Container Tests
// ===========================================================================

TEST_CASE("VAC2: write and read base index sections", "[analysis][vac2]") {
    const auto path = std::filesystem::temp_directory_path() / "voidplayer_test_base.vac";

    vr::analysis::Vac2BaseData data;
    data.codec = AnalysisCodec::VVC;
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
    unit0.unit_kind = static_cast<uint8_t>(AnalysisUnitKind::Nalu);
    unit0.flags = VAC2_UNIT_FLAG_PARAMETER_SET;
    unit0.pset_snapshot = 0;

    Vac2BitstreamUnitEntry unit1{};
    unit1.packet_index = 0;
    unit1.au_index = 0;
    unit1.offset = 32;
    unit1.size = 68;
    unit1.nal_type = 7;
    unit1.temporal_id = 0;
    unit1.unit_kind = static_cast<uint8_t>(AnalysisUnitKind::Nalu);
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
    unit2.unit_kind = static_cast<uint8_t>(AnalysisUnitKind::Nalu);
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
    REQUIRE(vac2.header().codec == static_cast<uint16_t>(AnalysisCodec::VVC));
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

TEST_CASE("VAC2: parser rejects undefined codec values", "[analysis][vac2]") {
    const auto path = std::filesystem::temp_directory_path() /
        "voidplayer_test_base_bad_codec.vac";
    REQUIRE(vr::analysis::write_vac2_base_container(
        path.string(), make_vacache_base_data(88)));

    overwrite_u16(path, offsetof(Vac2Header, codec), 0xffff);

    vr::analysis::Vac2BaseFile vac2;
    REQUIRE_FALSE(vac2.open(path.string()));
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
    data.codec = AnalysisCodec::VVC;
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
    REQUIRE(chunk.header().codec == static_cast<uint16_t>(AnalysisCodec::VVC));
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
    data.codec = AnalysisCodec::HEVC;
    data.start_frame = 10;
    data.end_frame = 9;
    data.sections.push_back(vr::analysis::make_vachunk_string_section("META", "{}"));

    REQUIRE_FALSE(vr::analysis::write_vachunk_file(path.string(), data));

    data.start_frame = 10;
    data.end_frame = 10;
    REQUIRE_FALSE(vr::analysis::write_vachunk_file(path.string(), data, 8));
    std::filesystem::remove(path);
}

TEST_CASE("VACHUNK: record section factory guards narrow counts",
          "[analysis][vachunk]") {
    REQUIRE(vr::analysis::vachunk_record_section_fits(
        static_cast<size_t>(std::numeric_limits<uint32_t>::max()), 1));
    REQUIRE_FALSE(vr::analysis::vachunk_record_section_fits(
        static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1ull, 1));
    REQUIRE_FALSE(vr::analysis::vachunk_record_section_fits(
        1, static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1ull));

    std::vector<VachunkFrameSummary> empty;
    const auto section = vr::analysis::make_vachunk_record_section("FSUM", empty);
    REQUIRE(section.entry_size == sizeof(VachunkFrameSummary));
    REQUIRE(section.entry_count == 0);
    REQUIRE(section.decoded_size == 0);
    REQUIRE(section.bytes.empty());
}

TEST_CASE("VACHUNK: parser rejects undefined codec values", "[analysis][vachunk]") {
    const auto path = std::filesystem::temp_directory_path() /
        "voidplayer_test_chunk_bad_codec.vck";
    REQUIRE(vr::analysis::write_vachunk_file(path.string(), make_overlay_chunk(0, 0)));

    overwrite_u16(path, offsetof(VachunkHeader, codec), 0xffff);

    vr::analysis::VachunkFile chunk;
    REQUIRE_FALSE(chunk.open(path.string()));
    std::filesystem::remove(path);
}

TEST_CASE("VACHUNK: checksum fields are reserved zero in version 1",
          "[analysis][vachunk]") {
    const auto path = std::filesystem::temp_directory_path() /
        "voidplayer_test_chunk_reserved_checksum.vck";

    REQUIRE(vr::analysis::write_vachunk_file(path.string(), make_overlay_chunk(0, 0)));
    overwrite_u64(path, offsetof(VachunkHeader, checksum), 1);
    vr::analysis::VachunkFile header_chunk;
    REQUIRE_FALSE(header_chunk.open(path.string()));

    REQUIRE(vr::analysis::write_vachunk_file(path.string(), make_overlay_chunk(0, 0)));
    overwrite_u64(
        path,
        sizeof(VachunkHeader) + offsetof(VachunkSectionEntry, checksum),
        1);
    vr::analysis::VachunkFile section_chunk;
    REQUIRE_FALSE(section_chunk.open(path.string()));

    std::filesystem::remove(path);
}

TEST_CASE("VACHUNK: zstd section compression roundtrips",
          "[analysis][vachunk][zstd]") {
    const auto path = std::filesystem::temp_directory_path() /
        "voidplayer_test_compressed.vck";

    std::vector<VachunkFrameSummary> summaries(256);
    for (size_t i = 0; i < summaries.size(); ++i) {
        summaries[i].poc = static_cast<int32_t>(i);
        summaries[i].coded_order = static_cast<uint32_t>(i);
        summaries[i].avg_qp = 24;
        summaries[i].qp_min = 24;
        summaries[i].qp_max = 24;
    }

    vr::analysis::VachunkData data;
    data.kind = VachunkKind::FrameSummaryExact;
    data.codec = AnalysisCodec::HEVC;
    data.base_content_revision = 5;
    data.generator_revision = 9;
    data.start_frame = 0;
    data.end_frame = static_cast<uint32_t>(summaries.size() - 1);
    data.sections.push_back(
        vr::analysis::make_vachunk_record_section("FSUM", summaries));

    REQUIRE(vr::analysis::write_vachunk_file(path.string(), data));

    vr::analysis::VachunkFile chunk;
    REQUIRE(chunk.open(path.string()));
    REQUIRE(chunk.header().compression == VACHUNK_COMPRESSION_ZSTD);
    REQUIRE(chunk.section("FSUM") != nullptr);
    REQUIRE((chunk.section("FSUM")->flags & VACHUNK_SECTION_FLAG_ZSTD) != 0);
    REQUIRE(chunk.section("FSUM")->size < chunk.section("FSUM")->decoded_size);

    std::vector<uint8_t> raw;
    REQUIRE(chunk.read_section("FSUM", raw));
    REQUIRE(raw.size() == summaries.size() * sizeof(VachunkFrameSummary));
    const auto* decoded = reinterpret_cast<const VachunkFrameSummary*>(raw.data());
    REQUIRE(decoded[17].poc == 17);
    REQUIRE(decoded[17].avg_qp == 24);

    vr::analysis::VachunkData roundtrip;
    REQUIRE(chunk.read_data(roundtrip));
    REQUIRE(roundtrip.sections.size() == 1);
    REQUIRE((roundtrip.sections[0].flags & VACHUNK_SECTION_FLAG_ZSTD) == 0);
    REQUIRE(roundtrip.sections[0].bytes == raw);

    std::filesystem::remove(path);
}

TEST_CASE("VACHUNK: overlay chunk carries frame data",
          "[analysis][vachunk][overlay]") {
    namespace fs = std::filesystem;
    const auto chunk_data = make_overlay_chunk(0, 1);
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
    REQUIRE(frame0.summary.avg_qp == 22);
    REQUIRE(frame0.summary.num_cus == 1);
    REQUIRE(frame0.cus.size() == 1);
    if (!frame0.cus.empty()) {
        REQUIRE(frame0.cus[0].common.x == 0);
        REQUIRE(frame0.cus[0].common.y == 0);
        REQUIRE(frame0.cus[0].common.qp == 22);
        REQUIRE(frame0.cus[0].common.pred_mode == 1);
        REQUIRE(frame0.cus[0].common.bit_count == 128);
    }

    vr::analysis::DecodedOverlayChunk decoded_chunk;
    REQUIRE(vr::analysis::read_overlay_vachunk_chunk(chunk, decoded_chunk));
    vr::analysis::VachunkOverlayFrameData decoded_frame1;
    REQUIRE(vr::analysis::read_overlay_vachunk_frame(
        decoded_chunk,
        1,
        decoded_frame1));
    REQUIRE(decoded_frame1.summary.avg_qp == 23);
    REQUIRE(decoded_frame1.cus.size() == 1);
    if (!decoded_frame1.cus.empty()) {
        REQUIRE(decoded_frame1.cus[0].common.qp == 23);
    }

    vr::analysis::VachunkOverlayFrameData missing;
    REQUIRE_FALSE(vr::analysis::read_overlay_vachunk_frame(chunk, 2, missing));

    chunk.close();
    fs::remove(path);

    const auto window_chunk_data = make_overlay_chunk(10, 11);
    REQUIRE(window_chunk_data.start_frame == 10);
    REQUIRE(window_chunk_data.end_frame == 11);

    const auto window_path =
        fs::temp_directory_path() / "voidplayer_test_overlay_window.vck";
    fs::remove(window_path);
    REQUIRE(vr::analysis::write_vachunk_file(window_path.string(), window_chunk_data));

    vr::analysis::VachunkFile window_chunk;
    REQUIRE(window_chunk.open(window_path.string()));
    REQUIRE(window_chunk.header().start_frame == 10);
    REQUIRE(window_chunk.header().end_frame == 11);
    vr::analysis::VachunkOverlayFrameData frame10;
    REQUIRE(vr::analysis::read_overlay_vachunk_frame(window_chunk, 10, frame10));
    REQUIRE(frame10.summary.avg_qp == 22);
    REQUIRE(frame10.cus.size() == 1);
    REQUIRE_FALSE(vr::analysis::read_overlay_vachunk_frame(window_chunk, 0, missing));
    window_chunk.close();
    fs::remove(window_path);
}

TEST_CASE("Overlay raster: fills BGRA spans and guards invalid surfaces",
          "[analysis][overlay][raster]") {
    std::vector<uint8_t> pixels(8, 0);
    vr::analysis::OverlayRasterStats stats;
    vr::analysis::fill_overlay_rect(
        pixels,
        2,
        1,
        0,
        0,
        2,
        1,
        vr::analysis::OverlayColor{1, 2, 3, 4},
        &stats);
    REQUIRE(stats.filled_pixels == 2);
    REQUIRE(pixels == std::vector<uint8_t>{1, 2, 3, 4, 1, 2, 3, 4});

    const std::vector<uint8_t> sentinel{9, 9, 9, 9};
    std::vector<uint8_t> invalid = sentinel;
    vr::analysis::blend_overlay_pixel(
        invalid,
        0,
        1,
        0,
        0,
        vr::analysis::OverlayColor{1, 2, 3, 4});
    vr::analysis::fill_overlay_rect(
        invalid,
        -1,
        1,
        0,
        0,
        1,
        1,
        vr::analysis::OverlayColor{1, 2, 3, 4},
        &stats);
    vr::analysis::stroke_overlay_rect_mask(invalid, 0, 0, 0, 0, 1, 1);
    vr::analysis::stroke_overlay_rect_mask8(invalid, 0, 0, 0, 0, 1, 1);
    REQUIRE(invalid == sentinel);

    vr::analysis::VachunkOverlayFrameData frame;
    std::vector<uint8_t> heatmap = sentinel;
    REQUIRE_FALSE(vr::analysis::raster_overlay_heatmap(
        frame,
        1,
        1,
        0,
        1,
        vr::analysis::OverlayHeatmapMode::Qp,
        255,
        heatmap));
    REQUIRE(heatmap == sentinel);

    REQUIRE_FALSE(vr::analysis::raster_overlay_heatmap(
        frame,
        1,
        1,
        std::numeric_limits<int>::max(),
        std::numeric_limits<int>::max(),
        vr::analysis::OverlayHeatmapMode::Qp,
        255,
        heatmap));
    REQUIRE(heatmap == sentinel);
}

TEST_CASE("AnalysisManager: reads VAC2 base with overlay chunks",
          "[analysis][manager][vac2][vachunk]") {
    namespace fs = std::filesystem;
    auto& test_data = AnalysisTestData::instance();
    REQUIRE(test_data.ensure());

    const auto root = fs::temp_directory_path() / "voidplayer_manager_vac2";
    const auto hash_dir = root / "hash";
    const auto overlay_dir = hash_dir / "chunks" / "overlay";
    fs::remove_all(root);
    REQUIRE(fs::create_directories(overlay_dir));

    vr::analysis::Vac2BaseData base;
    base.codec = AnalysisCodec::H264;
    base.time_base_num = 1;
    base.time_base_den = 1000000;
    base.width = 1920;
    base.height = 1080;
    base.content_revision = 17;
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

    vr::analysis::AnalysisManager manager;
    REQUIRE(manager.load(base_path.string()));
    REQUIRE(manager.frame_count() == 2);
    REQUIRE(manager.video_width() == 1920);
    REQUIRE(manager.video_height() == 1080);
    REQUIRE(manager.current_frame_idx(45000) == 1);
    REQUIRE(manager.read_overlay_frame(0).cus.empty());

    auto stale_revision_chunk = make_overlay_chunk(0, 1);
    stale_revision_chunk.base_content_revision = base.content_revision + 1;
    REQUIRE(vr::analysis::write_vachunk_file(
        (overlay_dir / "overlay_stale_revision.vck").string(),
        stale_revision_chunk));
    REQUIRE(manager.read_overlay_frame(0).cus.empty());

    auto stale_codec_chunk = make_overlay_chunk(0, 1, AnalysisCodec::HEVC);
    stale_codec_chunk.base_content_revision = base.content_revision;
    REQUIRE(vr::analysis::write_vachunk_file(
        (overlay_dir / "overlay_stale_codec.vck").string(),
        stale_codec_chunk));
    REQUIRE(manager.read_overlay_frame(0).cus.empty());

    auto stale_feature_chunk = make_overlay_chunk(0, 1);
    stale_feature_chunk.base_content_revision = base.content_revision;
    stale_feature_chunk.feature_flags = 0;
    REQUIRE(vr::analysis::write_vachunk_file(
        (overlay_dir / "overlay_stale_features.vck").string(),
        stale_feature_chunk));
    REQUIRE(manager.read_overlay_frame(0).cus.empty());

    auto chunk_data = make_overlay_chunk(0, 1);
    chunk_data.base_content_revision = base.content_revision;
    const auto chunk_path = overlay_dir / "overlay_00000000_00000001_g1.vck";
    REQUIRE(vr::analysis::write_vachunk_file(
        chunk_path.string(),
        chunk_data));

    const auto frame0 = manager.read_overlay_frame(0);
    REQUIRE(frame0.summary.avg_qp == 22);
    REQUIRE(frame0.cus.size() == 1);
    if (!frame0.cus.empty()) {
        REQUIRE(frame0.cus[0].common.qp == 22);
    }

    auto replacement_chunk_data = make_overlay_chunk(0, 2);
    replacement_chunk_data.base_content_revision = base.content_revision;
    replacement_chunk_data.generator_revision = chunk_data.generator_revision;
    REQUIRE(set_overlay_frame_qp(replacement_chunk_data, 0, 56));
    REQUIRE(set_overlay_frame_qp(replacement_chunk_data, 1, 57));
    REQUIRE(vr::analysis::write_vachunk_file(
        chunk_path.string(),
        replacement_chunk_data));
    const auto replaced_frame0 = manager.read_overlay_frame(0);
    REQUIRE(replaced_frame0.summary.avg_qp == 22);
    REQUIRE(replaced_frame0.cus.size() == 1);
    if (!replaced_frame0.cus.empty()) {
        REQUIRE(replaced_frame0.cus[0].common.qp == 22);
    }
    manager.unload();
    REQUIRE(manager.load(base_path.string()));
    const auto reloaded_frame0 = manager.read_overlay_frame(0);
    REQUIRE(reloaded_frame0.summary.avg_qp == 56);
    REQUIRE(reloaded_frame0.cus.size() == 1);
    if (!reloaded_frame0.cus.empty()) {
        REQUIRE(reloaded_frame0.cus[0].common.qp == 56);
    }
    const auto replaced_frame1 = manager.read_overlay_frame(1);
    REQUIRE(replaced_frame1.summary.avg_qp == 57);
    REQUIRE(replaced_frame1.cus.size() == 1);
    if (!replaced_frame1.cus.empty()) {
        REQUIRE(replaced_frame1.cus[0].common.qp == 57);
    }

    auto newer_chunk_data = make_overlay_chunk(0, 1);
    newer_chunk_data.base_content_revision = base.content_revision;
    newer_chunk_data.generator_revision = chunk_data.generator_revision + 1;
    for (auto& section : newer_chunk_data.sections) {
        const std::string type(section.type, section.type + 4);
        if (type != "FSUM" && type != "CU4R") continue;
        if (type == "FSUM") {
            VachunkFrameSummary summary{};
            std::memcpy(&summary, section.bytes.data(), sizeof(summary));
            summary.avg_qp = 41;
            summary.qp_min = 41;
            summary.qp_max = 41;
            std::memcpy(section.bytes.data(), &summary, sizeof(summary));
        } else if (type == "CU4R") {
            vr::analysis::VachunkCuRecord record{};
            std::memcpy(&record, section.bytes.data(), sizeof(record));
            record.common.qp = 41;
            std::memcpy(section.bytes.data(), &record, sizeof(record));
        }
    }
    REQUIRE(vr::analysis::write_vachunk_file(
        (overlay_dir / "overlay_00000000_00000001_g2.vck").string(),
        newer_chunk_data));
    manager.unload();
    REQUIRE(manager.load(base_path.string()));
    const auto newer_frame0 = manager.read_overlay_frame(0);
    REQUIRE(newer_frame0.summary.avg_qp == 41);
    REQUIRE(newer_frame0.cus.size() == 1);
    if (!newer_frame0.cus.empty()) {
        REQUIRE(newer_frame0.cus[0].common.qp == 41);
    }

    std::atomic<bool> stop_reader{false};
    std::atomic<int> read_count{0};
    std::thread reader([&] {
        while (!stop_reader.load(std::memory_order_acquire)) {
            (void)manager.frame_count();
            (void)manager.current_frame_idx(45000);
            (void)manager.read_overlay_frame(0);
            read_count.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 20; ++i) {
        REQUIRE(manager.load(base_path.string()));
        if ((i % 2) == 0) {
            manager.unload();
        }
    }
    stop_reader.store(true, std::memory_order_release);
    reader.join();
    REQUIRE(read_count.load(std::memory_order_relaxed) > 0);

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
    base.codec = AnalysisCodec::HEVC;
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
    unit.unit_kind = static_cast<uint8_t>(AnalysisUnitKind::Nalu);
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
    REQUIRE(base_file.header().codec == static_cast<uint16_t>(AnalysisCodec::HEVC));
    REQUIRE(base_file.header().packet_count == 1);

    vr::analysis::VachunkKey key;
    key.kind = VachunkKind::FrameSummaryExact;
    key.codec = AnalysisCodec::HEVC;
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

TEST_CASE("VACache: failed base publish leaves existing final path intact", "[analysis][vacache]") {
    const auto root = std::filesystem::temp_directory_path() /
        "voidplayer_vacache_base_publish_failure_test";
    std::filesystem::remove_all(root);

    vr::analysis::VacacheStore store(root.string(), "abc123");
    REQUIRE(store.ensure_layout());

    const auto final_path = std::filesystem::path(store.base_path());
    REQUIRE(std::filesystem::create_directory(final_path));

    REQUIRE_FALSE(store.write_base_atomic(make_vacache_base_data(88)));
    REQUIRE(std::filesystem::is_directory(final_path));
    REQUIRE(std::filesystem::is_empty(std::filesystem::path(store.tmp_dir())));

    std::filesystem::remove_all(root);
}

TEST_CASE("VACache: failed chunk publish leaves existing final path intact", "[analysis][vacache]") {
    const auto root = std::filesystem::temp_directory_path() /
        "voidplayer_vacache_chunk_publish_failure_test";
    std::filesystem::remove_all(root);

    vr::analysis::VacacheStore store(root.string(), "abc123");
    REQUIRE(store.ensure_layout());

    vr::analysis::VachunkKey key;
    key.kind = VachunkKind::FrameSummaryExact;
    key.codec = AnalysisCodec::HEVC;
    key.feature_flags = VACHUNK_FEATURE_QP;
    key.base_content_revision = 88;
    key.generator_revision = 3;
    key.start_frame = 0;
    key.end_frame = 0;
    key.start_packet = 0;
    key.end_packet = 0;
    key.start_unit = 0;
    key.end_unit = 0;

    REQUIRE(std::filesystem::create_directories(
        std::filesystem::path(store.chunks_dir(key.kind))));
    const auto final_path = std::filesystem::path(store.chunk_path(key));
    REQUIRE(std::filesystem::create_directory(final_path));

    REQUIRE_FALSE(store.write_chunk_atomic(key, make_frame_summary_chunk_data(27)));
    REQUIRE(std::filesystem::is_directory(final_path));
    REQUIRE(std::filesystem::is_empty(std::filesystem::path(store.tmp_dir())));

    std::filesystem::remove_all(root);
}
