#include "analysis/cache/overlay_chunk.h"

#include <cstring>
#include <limits>
#include <utility>

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
        section->decoded_size != static_cast<uint64_t>(section->entry_count) * sizeof(T) ||
        section->decoded_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!chunk.read_section(type, bytes) || bytes.size() != section->decoded_size) {
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
    DecodedOverlayChunk decoded;
    if (!read_overlay_vachunk_chunk(chunk, decoded)) {
        out = {};
        return false;
    }
    return read_overlay_vachunk_frame(decoded, frame_index, out);
}

bool read_overlay_vachunk_chunk(const VachunkFile& chunk,
                                DecodedOverlayChunk& out) {
    out = {};
    const auto& header = chunk.header();
    if (header.kind != static_cast<uint16_t>(VachunkKind::Overlay)) {
        return false;
    }

    DecodedOverlayChunk decoded;
    decoded.header = header;
    if (!read_record_section(chunk, "FSUM", decoded.summaries) ||
        !read_record_section(chunk, "FIDX", decoded.frame_index) ||
        !read_record_section(chunk, "CU4R", decoded.records) ||
        decoded.summaries.size() != decoded.frame_index.size()) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

bool read_overlay_vachunk_frame(const DecodedOverlayChunk& chunk,
                                uint32_t frame_index,
                                VachunkOverlayFrameData& out) {
    out = {};
    const auto& header = chunk.header;
    if (header.kind != static_cast<uint16_t>(VachunkKind::Overlay) ||
        frame_index < header.start_frame ||
        frame_index > header.end_frame) {
        return false;
    }

    const size_t local = static_cast<size_t>(frame_index - header.start_frame);
    if (local >= chunk.frame_index.size() ||
        local >= chunk.summaries.size() ||
        chunk.frame_index[local].frame_index != frame_index) {
        return false;
    }
    const auto& entry = chunk.frame_index[local];
    if ((entry.flags & (VACHUNK_OVERLAY_FRAME_FLAG_COMPLETE |
                        VACHUNK_OVERLAY_FRAME_FLAG_EXACT)) !=
            (VACHUNK_OVERLAY_FRAME_FLAG_COMPLETE |
             VACHUNK_OVERLAY_FRAME_FLAG_EXACT) ||
        entry.first_unit > chunk.records.size() ||
        entry.unit_count > chunk.records.size() - entry.first_unit) {
        return false;
    }

    out.summary = chunk.summaries[local];
    out.feature_flags = header.feature_flags;
    out.frame_flags = entry.flags;
    out.cus.insert(out.cus.end(),
                   chunk.records.begin() + entry.first_unit,
                   chunk.records.begin() + entry.first_unit + entry.unit_count);
    return true;
}

} // namespace vr::analysis
