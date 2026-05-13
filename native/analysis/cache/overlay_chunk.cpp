#include "analysis/cache/overlay_chunk.h"

#include <cstring>
#include <limits>

namespace vr::analysis {
namespace {

static_assert(sizeof(VachunkCuRecord) == VACHUNK_CU_SIZE_INTER);

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

    std::vector<VachunkFrameSummary> summaries;
    std::vector<VachunkOverlayFrameIndexEntry> index;
    std::vector<VachunkCuRecord> records;
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
