#pragma once

#include <cstdint>
#include <string>

namespace vr::analysis {

/// Single-pass VAC2 base generator using FFmpeg.
/// Opens the video file, iterates video packets, and writes the lightweight
/// VAC2 packet/unit/frame map directly — no legacy sidecar files.
class AnalysisGenerator {
public:
    /// Generate a VAC2 base index from the lightweight FFmpeg scan.
    /// Writes packet, bitstream-unit, frame, and lightweight frame-summary
    /// tables without running deep overlay analysis.
    static bool generate_vac2_base(const std::string& video_path,
                                   const std::string& vac2_path,
                                   uint64_t max_output_bytes = 0);
};

} // namespace vr::analysis
