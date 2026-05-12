#pragma once

#include "analysis/parsers/vachunk_parser.h"
#include "analysis/parsers/vbs4_parser.h"

#include <cstdint>
#include <vector>

namespace vr::analysis {

struct VachunkOverlayFrameData {
    Vbs4FrameSummary summary{};
    std::vector<VbsCuRecord> cus;
};

bool build_overlay_vachunk_from_vbs4(const Vbs4File& vbs4,
                                     uint32_t start_frame,
                                     uint32_t end_frame,
                                     VachunkData& out);

bool read_overlay_vachunk_frame(const VachunkFile& chunk,
                                uint32_t frame_index,
                                VachunkOverlayFrameData& out);

} // namespace vr::analysis
