#pragma once

#include "analysis/parsers/binary_types.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace vr::analysis {

struct VbsCuRecord {
    VbsCuCommon common;
    union {
        VbsCuIntra intra;
        VbsCuInter inter;
    };
};

struct Vbs3FrameData {
    Vbs3FrameSummary summary;
    std::vector<VbsCuRecord> cus;
};

class Vbs3File {
public:
    bool open(const std::string& path);
    void close();

    const Vbs3Header& header() const { return header_; }
    int frame_count() const { return static_cast<int>(summaries_.size()); }
    const Vbs3SectionEntry* section(const char type[4]) const;
    const Vbs3CuIndexEntry& cu_index_entry(int i) const { return cu_index_[i]; }

    Vbs3FrameData read_frame(int frame_idx) const;
    Vbs3FrameSummary read_frame_summary(int frame_idx) const;
    std::vector<Vbs3FrameSummary> read_all_frame_summaries() const;

private:
    mutable std::ifstream file_;
    Vbs3Header header_{};
    std::vector<Vbs3SectionEntry> sections_;
    std::vector<Vbs3FrameSummary> summaries_;
    std::vector<Vbs3CuIndexEntry> cu_index_;
    uint64_t cu_blob_offset_ = 0;
    uint64_t cu_blob_size_ = 0;
};

} // namespace vr::analysis
