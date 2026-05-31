#pragma once

#include "analysis/parsers/vachunk_parser.h"

#include <cstdint>
#include <vector>

namespace vr::analysis {

struct VachunkCuRecord {
    VachunkCuCommon common;
    union {
        VachunkCuIntra intra;
        VachunkCuInter inter;
    };
};

struct VachunkOverlayFrameData {
    VachunkFrameSummary summary{};
    uint64_t feature_flags = 0;
    uint32_t frame_flags = 0;
    std::vector<VachunkCuRecord> cus;
};

struct DecodedOverlayChunk {
    VachunkHeader header{};
    std::vector<VachunkFrameSummary> summaries;
    std::vector<VachunkOverlayFrameIndexEntry> frame_index;
    std::vector<VachunkCuRecord> records;
};

bool read_overlay_vachunk_chunk(const VachunkFile& chunk,
                                DecodedOverlayChunk& out);

bool read_overlay_vachunk_frame(const DecodedOverlayChunk& chunk,
                                uint32_t frame_index,
                                VachunkOverlayFrameData& out);

bool read_overlay_vachunk_frame(const VachunkFile& chunk,
                                uint32_t frame_index,
                                VachunkOverlayFrameData& out);

} // namespace vr::analysis
