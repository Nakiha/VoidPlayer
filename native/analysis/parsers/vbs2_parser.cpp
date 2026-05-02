#include "analysis/parsers/vbs2_parser.h"
#include "common/win_utf8.h"

#include <fstream>
#include <limits>

namespace vr::analysis {

namespace {
constexpr uint32_t kMaxFrames = 10'000'000;
constexpr uint32_t kMaxCusPerFrame = 2'000'000;

bool validate_index_entry(const Vbs2IndexEntry& idx,
                          const Vbs2FrameHeader& frame_header,
                          uint64_t file_size) {
    if (idx.offset > file_size ||
        file_size - idx.offset < sizeof(Vbs2FrameHeader)) {
        return false;
    }
    if (idx.num_cus > kMaxCusPerFrame || frame_header.num_cus < 0) {
        return false;
    }
    if (static_cast<uint32_t>(frame_header.num_cus) != idx.num_cus) {
        return false;
    }
    if (frame_header.num_ref_l0 > 15 || frame_header.num_ref_l1 > 15) {
        return false;
    }

    const uint64_t min_frame_bytes =
        static_cast<uint64_t>(sizeof(Vbs2FrameHeader)) +
        static_cast<uint64_t>(idx.num_cus) *
            static_cast<uint64_t>(sizeof(Vbs2CuCommon));
    return min_frame_bytes <= file_size - idx.offset;
}
} // namespace

bool Vbs2File::open(const std::string& path) {
    index_.clear();

    file_.open(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!file_) return false;

    // Read header
    file_.read(reinterpret_cast<char*>(&header_), sizeof(Vbs2Header));
    if (!file_ || header_.magic[0] != 'V' || header_.magic[1] != 'B' ||
                 header_.magic[2] != 'S' || header_.magic[3] != '2') {
        file_.close();
        return false;
    }

    // Read frame index — validate bounds first
    if (header_.num_frames > kMaxFrames) {
        file_.close();
        return false;
    }
    size_t index_bytes = static_cast<size_t>(header_.num_frames) * sizeof(Vbs2IndexEntry);
    file_.seekg(0, std::ios::end);
    auto file_size = file_.tellg();
    if (file_size < 0 ||
        static_cast<std::streamoff>(header_.index_offset) > file_size ||
        static_cast<std::streamoff>(index_bytes) > file_size - static_cast<std::streamoff>(header_.index_offset)) {
        file_.close();
        return false;
    }

    file_.seekg(header_.index_offset);
    index_.resize(header_.num_frames);
    if (header_.num_frames > 0) {
        file_.read(reinterpret_cast<char*>(index_.data()),
                   static_cast<std::streamsize>(header_.num_frames * sizeof(Vbs2IndexEntry)));
        if (!file_) {
            index_.clear();
            file_.close();
            return false;
        }
    }

    const uint64_t file_size_u64 = static_cast<uint64_t>(file_size);
    for (const auto& idx : index_) {
        Vbs2FrameHeader fh{};
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(idx.offset));
        file_.read(reinterpret_cast<char*>(&fh), sizeof(Vbs2FrameHeader));
        if (!file_ || !validate_index_entry(idx, fh, file_size_u64)) {
            index_.clear();
            file_.close();
            return false;
        }
    }

    return true;
}

void Vbs2File::close() {
    file_.close();
    index_.clear();
    header_ = {};
}

Vbs2FrameHeader Vbs2File::read_frame_header(int frame_idx) const {
    Vbs2FrameHeader fh{};
    if (frame_idx < 0 || frame_idx >= static_cast<int>(index_.size())) return fh;

    file_.clear();
    file_.seekg(index_[frame_idx].offset);
    file_.read(reinterpret_cast<char*>(&fh), sizeof(Vbs2FrameHeader));
    return fh;
}

Vbs2FrameData Vbs2File::read_frame(int frame_idx) const {
    Vbs2FrameData result;
    if (frame_idx < 0 || frame_idx >= static_cast<int>(index_.size())) return result;

    const auto& idx = index_[frame_idx];
    file_.clear();
    file_.seekg(idx.offset);

    // Read frame header
    file_.read(reinterpret_cast<char*>(&result.header), sizeof(Vbs2FrameHeader));
    if (!file_ || result.header.num_cus < 0 ||
        static_cast<uint32_t>(result.header.num_cus) != idx.num_cus ||
        idx.num_cus > kMaxCusPerFrame) {
        return result;
    }

    // Read CU records
    result.cus.reserve(idx.num_cus);
    for (uint32_t i = 0; i < idx.num_cus; i++) {
        Vbs2CuRecord rec{};
        file_.read(reinterpret_cast<char*>(&rec.common), sizeof(Vbs2CuCommon));
        if (!file_) break;

        switch (rec.common.pred_mode) {
        case 1: // MODE_INTRA
            file_.read(reinterpret_cast<char*>(&rec.intra), sizeof(Vbs2CuIntra));
            break;
        case 0: // MODE_INTER
            file_.read(reinterpret_cast<char*>(&rec.inter), sizeof(Vbs2CuInter));
            break;
        default:
            // IBC (2) or PLT (3) — no extension, or skip unknown
            break;
        }
        result.cus.push_back(rec);
    }

    return result;
}

std::vector<Vbs2FrameHeader> Vbs2File::read_all_frame_headers() const {
    std::vector<Vbs2FrameHeader> headers;
    headers.reserve(index_.size());

    for (const auto& idx : index_) {
        file_.clear();
        file_.seekg(idx.offset);
        Vbs2FrameHeader fh;
        file_.read(reinterpret_cast<char*>(&fh), sizeof(Vbs2FrameHeader));
        if (!file_) break;
        headers.push_back(fh);
    }

    return headers;
}

} // namespace vr::analysis
