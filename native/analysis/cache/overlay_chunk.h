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
    std::vector<VachunkCuRecord> cus;
};

bool read_overlay_vachunk_frame(const VachunkFile& chunk,
                                uint32_t frame_index,
                                VachunkOverlayFrameData& out);

} // namespace vr::analysis
