#pragma once

#include "analysis/parsers/binary_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace vr::analysis {

class VbiFile {
public:
    bool open(const std::string& path);
    bool open_region(const std::string& path, uint64_t offset, uint64_t size);
    void close();

    const VbiHeader& header() const { return header_; }
    int nalu_count() const { return static_cast<int>(entries_.size()); }
    int unit_count() const { return static_cast<int>(entries_.size()); }
    VbiCodec codec() const { return static_cast<VbiCodec>(header_.codec); }
    VbiUnitKind unit_kind() const { return static_cast<VbiUnitKind>(header_.unit_kind); }

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
