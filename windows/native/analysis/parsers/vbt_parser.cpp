#include "analysis/parsers/vbt_parser.h"

#include <algorithm>
#include <fstream>

namespace vr::analysis {

bool VbtFile::open(const std::string& path) {
    entries_.clear();

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    // Read header
    f.read(reinterpret_cast<char*>(&header_), sizeof(VbtHeader));
    if (!f || header_.magic[0] != 'V' || header_.magic[1] != 'B' ||
             header_.magic[2] != 'T' || header_.magic[3] != '1') {
        return false;
    }

    // Read all entries
    entries_.resize(header_.num_packets);
    if (header_.num_packets > 0) {
        f.read(reinterpret_cast<char*>(entries_.data()),
               static_cast<std::streamsize>(header_.num_packets * sizeof(VbtEntry)));
        if (!f) {
            entries_.clear();
            return false;
        }
    }

    return true;
}

void VbtFile::close() {
    entries_.clear();
    header_ = {};
}

int VbtFile::packet_at_pts(int64_t pts) const {
    if (entries_.empty()) return -1;

    // Upper bound: first entry with pts > target
    auto it = std::upper_bound(entries_.begin(), entries_.end(), pts,
        [](int64_t val, const VbtEntry& e) { return val < e.pts; });

    if (it == entries_.begin()) return 0;
    return static_cast<int>(std::distance(entries_.begin(), it) - 1);
}

std::vector<int> VbtFile::keyframe_indices() const {
    std::vector<int> result;
    for (int i = 0; i < packet_count(); i++) {
        if (entries_[i].flags & VBT_FLAG_KEYFRAME) {
            result.push_back(i);
        }
    }
    return result;
}

} // namespace vr::analysis
