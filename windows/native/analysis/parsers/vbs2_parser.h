#pragma once

#include "analysis/parsers/binary_types.h"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace vr::analysis {

// One decoded CU record (common + mode-specific extension)
struct Vbs2CuRecord {
    Vbs2CuCommon common;
    union {
        Vbs2CuIntra intra;
        Vbs2CuInter inter;
    };
};

// A full frame: header + all CU records
struct Vbs2FrameData {
    Vbs2FrameHeader header;
    std::vector<Vbs2CuRecord> cus;
};

class Vbs2File {
public:
    bool open(const std::string& path);
    void close();

    const Vbs2Header& header() const { return header_; }
    int frame_count() const { return static_cast<int>(index_.size()); }
    const Vbs2IndexEntry& index_entry(int i) const { return index_[i]; }

    // Read full frame (header + all CU records)
    Vbs2FrameData read_frame(int frame_idx) const;

    // Read frame header only (no CU records — fast for trend charts)
    Vbs2FrameHeader read_frame_header(int frame_idx) const;

    // Read all frame headers without CU data
    std::vector<Vbs2FrameHeader> read_all_frame_headers() const;

private:
    mutable std::ifstream file_;
    Vbs2Header header_{};
    std::vector<Vbs2IndexEntry> index_;
};

} // namespace vr::analysis
