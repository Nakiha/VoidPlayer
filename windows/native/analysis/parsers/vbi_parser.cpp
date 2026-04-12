#include "analysis/parsers/vbi_parser.h"

#include <algorithm>
#include <fstream>

namespace vr::analysis {

bool VbiFile::open(const std::string& path) {
    entries_.clear();

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    // Read header
    f.read(reinterpret_cast<char*>(&header_), sizeof(VbiHeader));
    if (!f || header_.magic[0] != 'V' || header_.magic[1] != 'B' ||
             header_.magic[2] != 'I' || header_.magic[3] != '1') {
        return false;
    }

    // Read all entries
    entries_.resize(header_.num_nalus);
    if (header_.num_nalus > 0) {
        f.read(reinterpret_cast<char*>(entries_.data()),
               static_cast<std::streamsize>(header_.num_nalus * sizeof(VbiEntry)));
        if (!f) {
            entries_.clear();
            return false;
        }
    }

    return true;
}

void VbiFile::close() {
    entries_.clear();
    header_ = {};
}

std::vector<int> VbiFile::find_vcl_nalus() const {
    std::vector<int> result;
    for (int i = 0; i < nalu_count(); i++) {
        if (entries_[i].flags & VBI_FLAG_IS_VCL) {
            result.push_back(i);
        }
    }
    return result;
}

std::vector<int> VbiFile::find_keyframes() const {
    std::vector<int> result;
    for (int i = 0; i < nalu_count(); i++) {
        if (entries_[i].flags & VBI_FLAG_IS_KEYFRAME) {
            result.push_back(i);
        }
    }
    return result;
}

int VbiFile::nalu_for_offset(uint64_t byte_offset) const {
    if (entries_.empty()) return -1;

    // Upper bound: first entry with offset > target
    auto it = std::upper_bound(entries_.begin(), entries_.end(), byte_offset,
        [](uint64_t val, const VbiEntry& e) { return val < e.offset; });

    if (it == entries_.begin()) return 0;
    return static_cast<int>(std::distance(entries_.begin(), it) - 1);
}

} // namespace vr::analysis
