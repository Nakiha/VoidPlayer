#include "analysis/parsers/vbs2_parser.h"

#include <fstream>
#include <limits>

namespace vr::analysis {

bool Vbs2File::open(const std::string& path) {
    index_.clear();

    file_.open(path, std::ios::binary);
    if (!file_) return false;

    // Read header
    file_.read(reinterpret_cast<char*>(&header_), sizeof(Vbs2Header));
    if (!file_ || header_.magic[0] != 'V' || header_.magic[1] != 'B' ||
                 header_.magic[2] != 'S' || header_.magic[3] != '2') {
        file_.close();
        return false;
    }

    // Read frame index — validate bounds first
    constexpr uint32_t kMaxFrames = 10'000'000;
    if (header_.num_frames > kMaxFrames) return false;
    size_t index_bytes = static_cast<size_t>(header_.num_frames) * sizeof(Vbs2IndexEntry);
    file_.seekg(0, std::ios::end);
    auto file_size = file_.tellg();
    if (file_size < 0 || header_.index_offset < 0 ||
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
