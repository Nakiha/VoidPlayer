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

struct Vbs4FrameData {
    Vbs4FrameSummary summary;
    std::vector<VbsCuRecord> cus;
};

class Vbs4File {
public:
    bool open(const std::string& path);
    bool open_region(const std::string& path, uint64_t offset, uint64_t size);
    void close();

    const Vbs4Header& header() const { return header_; }
    int frame_count() const { return static_cast<int>(summaries_.size()); }
    const Vbs4SectionEntry* section(const char type[4]) const;

    Vbs4FrameData read_frame(int frame_idx) const;
    Vbs4FrameSummary read_frame_summary(int frame_idx) const;
    std::vector<Vbs4FrameSummary> read_all_frame_summaries() const;

private:
    struct DecodedBlockCache {
        uint32_t block_index = UINT32_MAX;
        std::vector<VbsCuRecord> records;
    };

    bool decode_block(uint32_t block_index, std::vector<VbsCuRecord>& out) const;

    mutable std::ifstream file_;
    uint64_t base_offset_ = 0;
    uint64_t region_size_ = 0;
    Vbs4Header header_{};
    std::vector<Vbs4SectionEntry> sections_;
    std::vector<Vbs4FrameSummary> summaries_;
    std::vector<Vbs4FrameIndexEntry> frame_index_;
    std::vector<Vbs4BlockIndexEntry> block_index_;
    uint64_t cpay_offset_ = 0;
    uint64_t cpay_size_ = 0;
    mutable DecodedBlockCache cache_;
};

} // namespace vr::analysis
