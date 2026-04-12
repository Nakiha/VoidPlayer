#pragma once

#include "analysis/parsers/binary_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace vr::analysis {

class VbtFile {
public:
    bool open(const std::string& path);
    void close();

    const VbtHeader& header() const { return header_; }
    int packet_count() const { return static_cast<int>(entries_.size()); }

    const VbtEntry& entry(int i) const { return entries_[i]; }
    const std::vector<VbtEntry>& entries() const { return entries_; }

    // Find packet index by PTS (largest index where pts <= target)
    int packet_at_pts(int64_t pts) const;

    // All keyframe packet indices
    std::vector<int> keyframe_indices() const;

private:
    VbtHeader header_{};
    std::vector<VbtEntry> entries_;
};

} // namespace vr::analysis
