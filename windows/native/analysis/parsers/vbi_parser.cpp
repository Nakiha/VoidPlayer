#include "analysis/parsers/vbi_parser.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace vr::analysis {

bool VbiFile::open(const std::string& path) {
    entries_.clear();
    header_ = {};

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char magic[4] = {};
    f.read(magic, sizeof(magic));
    if (!f || magic[0] != 'V' || magic[1] != 'B' || magic[2] != 'I') {
        return false;
    }

    if (magic[3] == '1') {
        VbiLegacyHeader legacy{};
        std::memcpy(legacy.magic, magic, sizeof(magic));
        f.read(reinterpret_cast<char*>(&legacy.num_nalus),
               sizeof(VbiLegacyHeader) - sizeof(magic));
        if (!f) return false;

        header_.magic[0] = 'V';
        header_.magic[1] = 'B';
        header_.magic[2] = 'I';
        header_.magic[3] = '1';
        header_.version = 1;
        header_.codec = static_cast<uint16_t>(VbiCodec::VVC);
        header_.unit_kind = static_cast<uint16_t>(VbiUnitKind::Nalu);
        header_.header_size = sizeof(VbiLegacyHeader);
        header_.num_units = legacy.num_nalus;
        header_.source_size = legacy.source_size;
    } else if (magic[3] == '2') {
        std::memcpy(header_.magic, magic, sizeof(magic));
        f.read(reinterpret_cast<char*>(&header_.version),
               sizeof(VbiHeader) - sizeof(magic));
        if (!f || header_.version != 2 || header_.header_size < sizeof(VbiHeader)) {
            return false;
        }
        if (header_.header_size > sizeof(VbiHeader)) {
            f.seekg(static_cast<std::streamoff>(header_.header_size - sizeof(VbiHeader)),
                    std::ios::cur);
            if (!f) return false;
        }
    } else {
        return false;
    }

    // Read all entries
    entries_.resize(header_.num_units);
    if (header_.num_units > 0) {
        f.read(reinterpret_cast<char*>(entries_.data()),
               static_cast<std::streamsize>(header_.num_units * sizeof(VbiEntry)));
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
