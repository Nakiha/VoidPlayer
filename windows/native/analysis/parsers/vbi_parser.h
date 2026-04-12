#pragma once

#include "analysis/parsers/binary_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace vr::analysis {

class VbiFile {
public:
    bool open(const std::string& path);
    void close();

    const VbiHeader& header() const { return header_; }
    int nalu_count() const { return static_cast<int>(entries_.size()); }

    const VbiEntry& entry(int i) const { return entries_[i]; }
    const std::vector<VbiEntry>& entries() const { return entries_; }

    // Indices of VCL NALUs (coded slice data)
    std::vector<int> find_vcl_nalus() const;

    // Indices of keyframe NALUs (IDR/CRA)
    std::vector<int> find_keyframes() const;

    // Find NALU index for a byte offset (largest index where entry offset <= target)
    int nalu_for_offset(uint64_t byte_offset) const;

private:
    VbiHeader header_{};
    std::vector<VbiEntry> entries_;
};

} // namespace vr::analysis
