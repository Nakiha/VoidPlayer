#include "analysis/cache/overlay_chunk.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <limits>

namespace vr::analysis {
namespace {

static_assert(sizeof(VbsCuRecord) == VBS_CU_SIZE_INTER);

template <typename T>
bool read_record_section(const VachunkFile& chunk,
                         const char (&type)[5],
                         std::vector<T>& out) {
    out.clear();
    const auto* section = chunk.section(type);
    if (!section ||
        section->entry_size != sizeof(T) ||
        section->decoded_size != section->size ||
        section->size != static_cast<uint64_t>(section->entry_count) * sizeof(T) ||
        section->size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!chunk.read_section(type, bytes) || bytes.size() != section->size) {
        return false;
    }
    out.resize(section->entry_count);
    if (!out.empty()) {
        std::memcpy(out.data(), bytes.data(), bytes.size());
    }
    return true;
}

} // namespace

bool build_overlay_vachunk_from_vbs4(const Vbs4File& vbs4,
                                     uint32_t start_frame,
                                     uint32_t end_frame,
                                     VachunkData& out) {
    out = {};
    if (vbs4.frame_count() <= 0 ||
        start_frame > end_frame ||
        end_frame >= static_cast<uint32_t>(vbs4.frame_count())) {
        return false;
    }

    std::vector<Vbs4FrameSummary> summaries;
    std::vector<VachunkOverlayFrameIndexEntry> frame_index;
    std::vector<VbsCuRecord> records;
    summaries.reserve(end_frame - start_frame + 1);
    frame_index.reserve(end_frame - start_frame + 1);

    for (uint32_t frame = start_frame; frame <= end_frame; ++frame) {
        const auto frame_data = vbs4.read_frame(static_cast<int>(frame));
        if (frame_data.summary.num_cus > 0 && frame_data.cus.empty()) {
            spdlog::error("[VACHUNK] failed to read VBS4 overlay frame: frame={}, num_cus={}, "
                          "cu_index_entry={}, coded_order={}",
                          frame,
                          frame_data.summary.num_cus,
                          frame_data.summary.cu_index_entry,
                          frame_data.summary.coded_order);
            out = {};
            return false;
        }

        VachunkOverlayFrameIndexEntry index{};
        index.frame_index = frame;
        index.first_unit = static_cast<uint32_t>(records.size());
        index.unit_count = static_cast<uint32_t>(frame_data.cus.size());
        index.flags =
            VACHUNK_OVERLAY_FRAME_FLAG_COMPLETE |
            VACHUNK_OVERLAY_FRAME_FLAG_EXACT;

        summaries.push_back(frame_data.summary);
        frame_index.push_back(index);
        records.insert(records.end(), frame_data.cus.begin(), frame_data.cus.end());
    }

    out.kind = VachunkKind::Overlay;
    out.codec = static_cast<VbiCodec>(vbs4.header().codec);
    out.feature_flags =
        VACHUNK_FEATURE_CU_GEOMETRY |
        VACHUNK_FEATURE_QP |
        VACHUNK_FEATURE_PRED_MODE |
        VACHUNK_FEATURE_MOTION_VECTORS |
        VACHUNK_FEATURE_REF_INDEXES |
        VACHUNK_FEATURE_BIT_COST;
    out.start_frame = start_frame;
    out.end_frame = end_frame;
    out.sections.push_back(make_vachunk_record_section("FSUM", summaries));
    out.sections.push_back(make_vachunk_record_section("FIDX", frame_index));
    out.sections.push_back(make_vachunk_record_section("CU4R", records));
    return true;
}

bool read_overlay_vachunk_frame(const VachunkFile& chunk,
                                uint32_t frame_index,
                                VachunkOverlayFrameData& out) {
    out = {};
    const auto& header = chunk.header();
    if (header.kind != static_cast<uint16_t>(VachunkKind::Overlay) ||
        frame_index < header.start_frame ||
        frame_index > header.end_frame) {
        return false;
    }

    std::vector<Vbs4FrameSummary> summaries;
    std::vector<VachunkOverlayFrameIndexEntry> index;
    std::vector<VbsCuRecord> records;
    if (!read_record_section(chunk, "FSUM", summaries) ||
        !read_record_section(chunk, "FIDX", index) ||
        !read_record_section(chunk, "CU4R", records) ||
        summaries.size() != index.size()) {
        return false;
    }

    const size_t local = static_cast<size_t>(frame_index - header.start_frame);
    if (local >= index.size() || index[local].frame_index != frame_index) {
        return false;
    }
    const auto& entry = index[local];
    if ((entry.flags & VACHUNK_OVERLAY_FRAME_FLAG_COMPLETE) == 0 ||
        entry.first_unit > records.size() ||
        entry.unit_count > records.size() - entry.first_unit) {
        return false;
    }

    out.summary = summaries[local];
    out.cus.insert(out.cus.end(),
                   records.begin() + entry.first_unit,
                   records.begin() + entry.first_unit + entry.unit_count);
    return true;
}

} // namespace vr::analysis
