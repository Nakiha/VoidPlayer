#pragma once

#include <cstdint>
#include <string>

namespace vr::analysis {

/// Single-pass VBI + VBT generator using FFmpeg.
/// Opens the video file, iterates all video packets, and writes both
/// binary index files in one pass — no Python, no subprocess calls.
class AnalysisGenerator {
public:
    /// Generate VBI (NALU index) and VBT (timestamps) files.
    /// Both files are written in a single FFmpeg pass.
    /// Returns true if at least VBT was generated successfully.
    static bool generate(const std::string& video_path,
                         const std::string& vbi_path,
                         const std::string& vbt_path);
};

} // namespace vr::analysis
