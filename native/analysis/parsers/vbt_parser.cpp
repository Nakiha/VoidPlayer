#include "analysis/parsers/vbt_parser.h"
#include "common/win_utf8.h"

#include <algorithm>
#include <fstream>
#include <limits>

namespace vr::analysis {

bool VbtFile::open(const std::string& path) {
    entries_.clear();
    pts_sorted_indices_.clear();
    header_ = {};

    std::ifstream f(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!f) return false;

    // Read header
    f.read(reinterpret_cast<char*>(&header_), sizeof(VbtHeader));
    if (!f || header_.magic[0] != 'V' || header_.magic[1] != 'B' ||
             header_.magic[2] != 'T' || header_.magic[3] != '1') {
        return false;
    }

    // Read all entries — validate bounds first
    constexpr uint32_t kMaxPackets = 10'000'000;
    if (header_.num_packets > kMaxPackets) return false;
    size_t entries_bytes = static_cast<size_t>(header_.num_packets) * sizeof(VbtEntry);
    f.seekg(0, std::ios::end);
    auto file_size = f.tellg();
    f.seekg(sizeof(VbtHeader));
    if (file_size < 0 ||
        static_cast<std::streamoff>(entries_bytes) > file_size - static_cast<std::streamoff>(sizeof(VbtHeader))) {
        return false;
    }

    entries_.resize(header_.num_packets);
    if (header_.num_packets > 0) {
        f.read(reinterpret_cast<char*>(entries_.data()),
               static_cast<std::streamsize>(header_.num_packets * sizeof(VbtEntry)));
        if (!f) {
            entries_.clear();
            pts_sorted_indices_.clear();
            return false;
        }
    }

    pts_sorted_indices_.reserve(entries_.size());
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        pts_sorted_indices_.push_back(i);
    }
    std::sort(pts_sorted_indices_.begin(), pts_sorted_indices_.end(),
              [this](int a, int b) {
                  if (entries_[a].pts != entries_[b].pts) {
                      return entries_[a].pts < entries_[b].pts;
                  }
                  return a < b;
              });

    return true;
}

void VbtFile::close() {
    entries_.clear();
    pts_sorted_indices_.clear();
    header_ = {};
}

int VbtFile::packet_at_pts(int64_t pts) const {
    if (entries_.empty() || pts_sorted_indices_.empty()) return -1;

    // Upper bound: first entry with pts > target
    auto it = std::upper_bound(pts_sorted_indices_.begin(), pts_sorted_indices_.end(), pts,
        [this](int64_t val, int packet_index) {
            return val < entries_[packet_index].pts;
        });

    if (it == pts_sorted_indices_.begin()) return pts_sorted_indices_.front();
    --it;
    return *it;
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
