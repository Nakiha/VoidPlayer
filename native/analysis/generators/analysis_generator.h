#pragma once

#include <cstdint>
#include <string>

namespace vr::analysis {

/// Single-pass VBI2 + VBT generator using FFmpeg.
/// Opens the video file, iterates all video packets, and writes both
/// binary index files in one pass — no Python, no subprocess calls.
class AnalysisGenerator {
public:
    /// Generate VBI2 (codec-specific bitstream unit index) and VBT files.
    /// Both files are written in a single FFmpeg pass.
    /// Returns true if at least VBT was generated successfully.
    static bool generate(const std::string& video_path,
                         const std::string& vbi_path,
                         const std::string& vbt_path,
                         uint64_t max_output_bytes = 0);

    /// Generate a VAC2 base index from the existing lightweight FFmpeg scan.
    ///
    /// This is the first migration bridge: it writes packet, bitstream-unit,
    /// frame, and lightweight frame-summary tables without running deep
    /// VBS4/overlay analysis.
    static bool generate_vac2_base(const std::string& video_path,
                                   const std::string& vac2_path,
                                   uint64_t max_output_bytes = 0);
};

} // namespace vr::analysis
