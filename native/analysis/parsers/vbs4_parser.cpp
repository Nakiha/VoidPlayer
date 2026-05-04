#include "analysis/parsers/vbs4_parser.h"
#include "common/win_utf8.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace vr::analysis {

namespace {
constexpr uint32_t kMaxFrames = 10'000'000;
constexpr uint32_t kMaxRecordsPerBlock = 16'000'000;
constexpr uint32_t kMaxCusPerFrame = 2'000'000;
constexpr uint32_t kMaxSections = 128;
constexpr uint32_t kMaxStreams = 128;

bool fourcc_eq(const char lhs[4], const char rhs[4]) {
    return std::memcmp(lhs, rhs, 4) == 0;
}

bool range_fits(uint64_t offset, uint64_t size, uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

bool section_records_fit(const Vbs4SectionEntry& section, uint32_t expected_entry_size) {
    return section.entry_size == expected_entry_size &&
           section.entry_count <= kMaxFrames &&
           section.size == static_cast<uint64_t>(section.entry_size) * section.entry_count;
}

bool read_bytes(std::ifstream& file, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) {
    if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) return false;
    out.resize(static_cast<size_t>(size));
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file) return false;
    if (out.empty()) return true;
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<size_t>(file.gcount()) == out.size();
}

uint32_t read_le_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

int8_t byte_to_i8(uint8_t value) {
    return value < 128 ? static_cast<int8_t>(value)
                       : static_cast<int8_t>(static_cast<int>(value) - 256);
}

bool read_uleb(const uint8_t*& cursor, const uint8_t* end, uint32_t& value) {
    uint32_t result = 0;
    uint32_t shift = 0;
    while (cursor < end && shift <= 28) {
        const uint8_t byte = *cursor++;
        result |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool decode_u32_stream(const uint8_t* data,
                       size_t size,
                       uint32_t count,
                       uint16_t encoding,
                       std::vector<uint32_t>& out) {
    out.clear();
    out.reserve(count);
    if (encoding == VBS4_ENCODING_RAW) {
        if (size < count) return false;
        for (uint32_t i = 0; i < count; ++i) out.push_back(data[i]);
        return true;
    }
    if (encoding == VBS4_ENCODING_BITSET) {
        if (size < (static_cast<size_t>(count) + 7) / 8) return false;
        for (uint32_t i = 0; i < count; ++i) {
            out.push_back((data[i >> 3] >> (i & 7)) & 1);
        }
        return true;
    }
    if (encoding == VBS4_ENCODING_ULEB128) {
        const uint8_t* cursor = data;
        const uint8_t* end = data + size;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t value = 0;
            if (!read_uleb(cursor, end, value)) return false;
            out.push_back(value);
        }
        return true;
    }
    if (encoding == VBS4_ENCODING_FRAME_PREFIX_U32) {
        if (size != static_cast<size_t>(count) * sizeof(uint32_t)) return false;
        for (uint32_t i = 0; i < count; ++i) {
            out.push_back(read_le_u32(data + static_cast<size_t>(i) * sizeof(uint32_t)));
        }
        return true;
    }
    return false;
}

bool decode_i32_stream(const uint8_t* data,
                       size_t size,
                       uint32_t count,
                       uint16_t encoding,
                       std::vector<int32_t>& out) {
    out.clear();
    out.reserve(count);
    if (encoding == VBS4_ENCODING_RAW) {
        if (size < count) return false;
        for (uint32_t i = 0; i < count; ++i) out.push_back(static_cast<int32_t>(data[i]));
        return true;
    }
    if (encoding == VBS4_ENCODING_SLEB128_ZIGZAG) {
        const uint8_t* cursor = data;
        const uint8_t* end = data + size;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t zz = 0;
            if (!read_uleb(cursor, end, zz)) return false;
            out.push_back(static_cast<int32_t>((zz >> 1) ^ (0u - (zz & 1u))));
        }
        return true;
    }
    std::vector<uint32_t> unsigned_values;
    if (!decode_u32_stream(data, size, count, encoding, unsigned_values)) return false;
    for (uint32_t value : unsigned_values) out.push_back(static_cast<int32_t>(value));
    return true;
}

struct DecodedStreams {
    std::vector<Vbs4StreamEntry> entries;
    std::vector<uint8_t> payload;

    const Vbs4StreamEntry* find(uint16_t id) const {
        for (const auto& entry : entries) {
            if (entry.stream_id == id) return &entry;
        }
        return nullptr;
    }

    bool u32(uint16_t id, uint32_t count, std::vector<uint32_t>& out) const {
        const auto* entry = find(id);
        if (!entry || entry->value_count != count ||
            !range_fits(entry->offset, entry->size, payload.size())) {
            return false;
        }
        return decode_u32_stream(payload.data() + entry->offset,
                                 entry->size,
                                 count,
                                 entry->encoding,
                                 out);
    }

    bool i32(uint16_t id, uint32_t count, std::vector<int32_t>& out) const {
        const auto* entry = find(id);
        if (!entry || entry->value_count != count ||
            !range_fits(entry->offset, entry->size, payload.size())) {
            return false;
        }
        return decode_i32_stream(payload.data() + entry->offset,
                                 entry->size,
                                 count,
                                 entry->encoding,
                                 out);
    }
};

uint8_t clamp_u8(int32_t value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint16_t clamp_u16(uint32_t value) {
    return static_cast<uint16_t>(std::min<uint32_t>(value, 0xFFFFu));
}

bool decode_hevc_records(const DecodedStreams& streams,
                         uint32_t record_count,
                         std::vector<VbsCuRecord>& out) {
    std::vector<uint32_t> x, y, log2_w, log2_h, depth, pred_mode, intra_mode;
    std::vector<uint32_t> mip_flag, isp_mode, skip, merge_flag, inter_dir, ref_l0, ref_l1;
    std::vector<int32_t> qp_delta, mv_l0_x, mv_l0_y, mv_l1_x, mv_l1_y;

    if (!streams.u32(VBS4_HEVC_X, record_count, x) ||
        !streams.u32(VBS4_HEVC_Y, record_count, y) ||
        !streams.u32(VBS4_HEVC_LOG2_W, record_count, log2_w) ||
        !streams.u32(VBS4_HEVC_LOG2_H, record_count, log2_h) ||
        !streams.u32(VBS4_HEVC_DEPTH, record_count, depth) ||
        !streams.u32(VBS4_HEVC_PRED_MODE, record_count, pred_mode) ||
        !streams.i32(VBS4_HEVC_QP_DELTA, record_count, qp_delta) ||
        !streams.u32(VBS4_HEVC_INTRA_MODE, record_count, intra_mode) ||
        !streams.u32(VBS4_HEVC_MIP_FLAG, record_count, mip_flag) ||
        !streams.u32(VBS4_HEVC_ISP_MODE, record_count, isp_mode) ||
        !streams.u32(VBS4_HEVC_SKIP_FLAG, record_count, skip) ||
        !streams.u32(VBS4_HEVC_MERGE_FLAG, record_count, merge_flag) ||
        !streams.u32(VBS4_HEVC_INTER_DIR, record_count, inter_dir) ||
        !streams.i32(VBS4_HEVC_MV_L0_X, record_count, mv_l0_x) ||
        !streams.i32(VBS4_HEVC_MV_L0_Y, record_count, mv_l0_y) ||
        !streams.i32(VBS4_HEVC_MV_L1_X, record_count, mv_l1_x) ||
        !streams.i32(VBS4_HEVC_MV_L1_Y, record_count, mv_l1_y) ||
        !streams.u32(VBS4_HEVC_REF_L0, record_count, ref_l0) ||
        !streams.u32(VBS4_HEVC_REF_L1, record_count, ref_l1)) {
        return false;
    }

    out.clear();
    out.reserve(record_count);
    int32_t qp = 0;
    for (uint32_t i = 0; i < record_count; ++i) {
        qp += qp_delta[i];
        VbsCuRecord rec{};
        rec.common.x = clamp_u16(x[i]);
        rec.common.y = clamp_u16(y[i]);
        rec.common.w = clamp_u8(1 << std::min<uint32_t>(log2_w[i], 7));
        rec.common.h = clamp_u8(1 << std::min<uint32_t>(log2_h[i], 7));
        rec.common.depth = clamp_u8(static_cast<int32_t>(depth[i]));
        rec.common.qp = clamp_u8(qp);
        rec.common.pred_mode = clamp_u8(static_cast<int32_t>(pred_mode[i]));
        if (rec.common.pred_mode == 1) {
            rec.intra.intra_mode = clamp_u8(static_cast<int32_t>(intra_mode[i]));
            rec.intra.mip_flag = clamp_u8(static_cast<int32_t>(mip_flag[i]));
            rec.intra.isp_mode = clamp_u8(static_cast<int32_t>(isp_mode[i]));
        } else {
            rec.inter.skip = clamp_u8(static_cast<int32_t>(skip[i]));
            rec.inter.merge_flag = clamp_u8(static_cast<int32_t>(merge_flag[i]));
            rec.inter.inter_dir = clamp_u8(static_cast<int32_t>(inter_dir[i]));
            rec.inter.mv_l0_x = static_cast<int16_t>(std::clamp(mv_l0_x[i], -32768, 32767));
            rec.inter.mv_l0_y = static_cast<int16_t>(std::clamp(mv_l0_y[i], -32768, 32767));
            rec.inter.mv_l1_x = static_cast<int16_t>(std::clamp(mv_l1_x[i], -32768, 32767));
            rec.inter.mv_l1_y = static_cast<int16_t>(std::clamp(mv_l1_y[i], -32768, 32767));
            rec.inter.ref_l0 = byte_to_i8(static_cast<uint8_t>(ref_l0[i]));
            rec.inter.ref_l1 = byte_to_i8(static_cast<uint8_t>(ref_l1[i]));
        }
        out.push_back(rec);
    }
    return true;
}

bool decode_h264_records(const DecodedStreams& streams,
                         uint32_t record_count,
                         uint32_t width,
                         std::vector<VbsCuRecord>& out) {
    std::vector<uint32_t> is_intra, skip, merge_flag, inter_dir, intra_mode, ref_l0, ref_l1;
    std::vector<int32_t> qp_delta, mv_l0_x, mv_l0_y, mv_l1_x, mv_l1_y;

    if (!streams.u32(VBS4_H264_IS_INTRA, record_count, is_intra) ||
        !streams.u32(VBS4_H264_SKIP_FLAG, record_count, skip) ||
        !streams.u32(VBS4_H264_MERGE_FLAG, record_count, merge_flag) ||
        !streams.u32(VBS4_H264_INTER_DIR, record_count, inter_dir) ||
        !streams.i32(VBS4_H264_QP_DELTA, record_count, qp_delta) ||
        !streams.u32(VBS4_H264_INTRA_MODE, record_count, intra_mode) ||
        !streams.u32(VBS4_H264_REF_L0, record_count, ref_l0) ||
        !streams.u32(VBS4_H264_REF_L1, record_count, ref_l1) ||
        !streams.i32(VBS4_H264_MV_L0_X, record_count, mv_l0_x) ||
        !streams.i32(VBS4_H264_MV_L0_Y, record_count, mv_l0_y) ||
        !streams.i32(VBS4_H264_MV_L1_X, record_count, mv_l1_x) ||
        !streams.i32(VBS4_H264_MV_L1_Y, record_count, mv_l1_y)) {
        return false;
    }

    const uint32_t mb_width = std::max<uint32_t>((width + 15) / 16, 1);
    out.clear();
    out.reserve(record_count);
    int32_t qp = 0;
    for (uint32_t i = 0; i < record_count; ++i) {
        qp += qp_delta[i];
        VbsCuRecord rec{};
        rec.common.x = clamp_u16((i % mb_width) * 16);
        rec.common.y = clamp_u16((i / mb_width) * 16);
        rec.common.w = 16;
        rec.common.h = 16;
        rec.common.depth = 0;
        rec.common.qp = clamp_u8(qp);
        rec.common.pred_mode = is_intra[i] ? 1 : 0;
        if (rec.common.pred_mode == 1) {
            rec.intra.intra_mode = clamp_u8(static_cast<int32_t>(intra_mode[i]));
        } else {
            rec.inter.skip = clamp_u8(static_cast<int32_t>(skip[i]));
            rec.inter.merge_flag = clamp_u8(static_cast<int32_t>(merge_flag[i]));
            rec.inter.inter_dir = clamp_u8(static_cast<int32_t>(inter_dir[i]));
            rec.inter.mv_l0_x = static_cast<int16_t>(std::clamp(mv_l0_x[i], -32768, 32767));
            rec.inter.mv_l0_y = static_cast<int16_t>(std::clamp(mv_l0_y[i], -32768, 32767));
            rec.inter.mv_l1_x = static_cast<int16_t>(std::clamp(mv_l1_x[i], -32768, 32767));
            rec.inter.mv_l1_y = static_cast<int16_t>(std::clamp(mv_l1_y[i], -32768, 32767));
            rec.inter.ref_l0 = byte_to_i8(static_cast<uint8_t>(ref_l0[i]));
            rec.inter.ref_l1 = byte_to_i8(static_cast<uint8_t>(ref_l1[i]));
        }
        out.push_back(rec);
    }
    return true;
}

bool validate_summary(const Vbs4FrameSummary& summary, size_t frame_index_count) {
    if (summary.num_ref_l0 > 15 || summary.num_ref_l1 > 15) return false;
    if (summary.num_cus > kMaxCusPerFrame) return false;
    if (summary.cu_index_entry >= frame_index_count) return false;
    if (summary.avg_qp > 63 || summary.qp_min > 63 || summary.qp_max > 63) return false;
    if (summary.num_cus > 0 && summary.qp_min > summary.qp_max) return false;
    return true;
}
} // namespace

bool Vbs4File::open(const std::string& path) {
    close();
    std::ifstream f(win_utf8::path_from_utf8(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto size = f.tellg();
    if (size < 0) return false;
    f.close();
    return open_region(path, 0, static_cast<uint64_t>(size));
}

bool Vbs4File::open_region(const std::string& path, uint64_t offset, uint64_t size) {
    close();

    file_.open(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!file_) return false;

    file_.seekg(0, std::ios::end);
    const auto file_size_pos = file_.tellg();
    if (file_size_pos < 0 ||
        offset > static_cast<uint64_t>(file_size_pos) ||
        size > static_cast<uint64_t>(file_size_pos) - offset) {
        close();
        return false;
    }

    const uint64_t file_size = size;
    base_offset_ = offset;
    region_size_ = size;
    file_.seekg(static_cast<std::streamoff>(base_offset_));
    file_.read(reinterpret_cast<char*>(&header_), sizeof(Vbs4Header));
    if (!file_ ||
        header_.magic[0] != 'V' ||
        header_.magic[1] != 'B' ||
        header_.magic[2] != 'S' ||
        header_.magic[3] != '4') {
        close();
        return false;
    }

    if (header_.version_major != 4 ||
        header_.header_size != sizeof(Vbs4Header) ||
        header_.section_entry_size != sizeof(Vbs4SectionEntry) ||
        header_.frame_count > kMaxFrames ||
        header_.block_count > kMaxFrames ||
        header_.section_count == 0 ||
        header_.section_count > kMaxSections ||
        header_.file_size != file_size ||
        !range_fits(header_.section_table_offset,
                    static_cast<uint64_t>(header_.section_count) * header_.section_entry_size,
                    file_size)) {
        close();
        return false;
    }

    sections_.resize(header_.section_count);
    file_.seekg(static_cast<std::streamoff>(base_offset_ + header_.section_table_offset));
    file_.read(reinterpret_cast<char*>(sections_.data()),
               static_cast<std::streamsize>(sections_.size() * sizeof(Vbs4SectionEntry)));
    if (!file_) {
        close();
        return false;
    }

    const auto* fsum = section("FSUM");
    const auto* fidx = section("FIDX");
    const auto* bidx = section("BIDX");
    const auto* cpay = section("CPAY");
    if (!fsum || !fidx || !bidx || !cpay ||
        !section_records_fit(*fsum, sizeof(Vbs4FrameSummary)) ||
        !section_records_fit(*fidx, sizeof(Vbs4FrameIndexEntry)) ||
        !section_records_fit(*bidx, sizeof(Vbs4BlockIndexEntry)) ||
        fsum->entry_count != header_.frame_count ||
        fidx->entry_count != header_.frame_count ||
        bidx->entry_count != header_.block_count ||
        cpay->entry_size != 0 ||
        cpay->entry_count != header_.block_count ||
        !range_fits(fsum->offset, fsum->size, file_size) ||
        !range_fits(fidx->offset, fidx->size, file_size) ||
        !range_fits(bidx->offset, bidx->size, file_size) ||
        !range_fits(cpay->offset, cpay->size, file_size)) {
        close();
        return false;
    }

    summaries_.resize(fsum->entry_count);
    file_.seekg(static_cast<std::streamoff>(base_offset_ + fsum->offset));
    if (!summaries_.empty()) {
        file_.read(reinterpret_cast<char*>(summaries_.data()),
                   static_cast<std::streamsize>(summaries_.size() * sizeof(Vbs4FrameSummary)));
    }

    frame_index_.resize(fidx->entry_count);
    file_.seekg(static_cast<std::streamoff>(base_offset_ + fidx->offset));
    if (!frame_index_.empty()) {
        file_.read(reinterpret_cast<char*>(frame_index_.data()),
                   static_cast<std::streamsize>(frame_index_.size() * sizeof(Vbs4FrameIndexEntry)));
    }

    block_index_.resize(bidx->entry_count);
    file_.seekg(static_cast<std::streamoff>(base_offset_ + bidx->offset));
    if (!block_index_.empty()) {
        file_.read(reinterpret_cast<char*>(block_index_.data()),
                   static_cast<std::streamsize>(block_index_.size() * sizeof(Vbs4BlockIndexEntry)));
    }
    if (!file_) {
        close();
        return false;
    }

    cpay_offset_ = cpay->offset;
    cpay_size_ = cpay->size;

    for (size_t i = 0; i < summaries_.size(); ++i) {
        if (!validate_summary(summaries_[i], frame_index_.size())) {
            close();
            return false;
        }
        const auto& idx = frame_index_[summaries_[i].cu_index_entry];
        if (idx.block_index >= block_index_.size() || idx.record_count != summaries_[i].num_cus) {
            close();
            return false;
        }
    }

    for (const auto& idx : block_index_) {
        if (idx.record_count > kMaxRecordsPerBlock ||
            idx.compression > VBS4_COMPRESSION_ZSTD ||
            !range_fits(idx.payload_offset, idx.payload_size, cpay_size_) ||
            idx.decoded_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            close();
            return false;
        }
    }

    return true;
}

void Vbs4File::close() {
    file_.close();
    base_offset_ = 0;
    region_size_ = 0;
    header_ = {};
    sections_.clear();
    summaries_.clear();
    frame_index_.clear();
    block_index_.clear();
    cpay_offset_ = 0;
    cpay_size_ = 0;
    cache_ = {};
}

const Vbs4SectionEntry* Vbs4File::section(const char type[4]) const {
    for (const auto& entry : sections_) {
        if (fourcc_eq(entry.type, type)) return &entry;
    }
    return nullptr;
}

Vbs4FrameSummary Vbs4File::read_frame_summary(int frame_idx) const {
    if (frame_idx < 0 || frame_idx >= static_cast<int>(summaries_.size())) {
        return {};
    }
    return summaries_[frame_idx];
}

bool Vbs4File::decode_block(uint32_t block_index, std::vector<VbsCuRecord>& out) const {
    if (cache_.block_index == block_index) {
        out = cache_.records;
        return true;
    }
    if (block_index >= block_index_.size()) return false;

    const auto& idx = block_index_[block_index];
    std::vector<uint8_t> stored;
    if (!read_bytes(file_,
                    base_offset_ + cpay_offset_ + idx.payload_offset,
                    idx.payload_size,
                    stored)) {
        return false;
    }

    std::vector<uint8_t> decoded;
    if (idx.compression == VBS4_COMPRESSION_ZSTD) {
        decoded.resize(static_cast<size_t>(idx.decoded_size));
        const size_t result = ZSTD_decompress(decoded.data(),
                                              decoded.size(),
                                              stored.data(),
                                              stored.size());
        if (ZSTD_isError(result) || result != decoded.size()) return false;
    } else if (idx.compression == VBS4_COMPRESSION_NONE) {
        if (stored.size() != static_cast<size_t>(idx.decoded_size)) return false;
        decoded = std::move(stored);
    } else {
        return false;
    }

    if (decoded.size() < sizeof(Vbs4DecodedBlockHeader)) return false;
    Vbs4DecodedBlockHeader block_header{};
    std::memcpy(&block_header, decoded.data(), sizeof(block_header));
    if (block_header.magic[0] != 'B' ||
        block_header.magic[1] != 'L' ||
        block_header.magic[2] != 'K' ||
        block_header.magic[3] != '4' ||
        block_header.header_size != sizeof(Vbs4DecodedBlockHeader) ||
        block_header.stream_entry_size != sizeof(Vbs4StreamEntry) ||
        block_header.stream_count == 0 ||
        block_header.stream_count > kMaxStreams ||
        block_header.record_count != idx.record_count ||
        block_header.frame_count != idx.frame_count ||
        !range_fits(block_header.header_size,
                    static_cast<uint64_t>(block_header.stream_count) * block_header.stream_entry_size,
                    decoded.size())) {
        return false;
    }

    DecodedStreams streams;
    streams.payload = std::move(decoded);
    streams.entries.resize(block_header.stream_count);
    std::memcpy(streams.entries.data(),
                streams.payload.data() + block_header.header_size,
                static_cast<size_t>(block_header.stream_count) * sizeof(Vbs4StreamEntry));

    std::vector<uint32_t> frame_prefix;
    if (!streams.u32(VBS4_STREAM_FRAME_PREFIX,
                     block_header.frame_count + 1,
                     frame_prefix) ||
        frame_prefix.empty() ||
        frame_prefix.back() != block_header.record_count) {
        return false;
    }

    const bool ok = header_.codec == static_cast<uint16_t>(VbiCodec::H264)
        ? decode_h264_records(streams, block_header.record_count, header_.width, out)
        : decode_hevc_records(streams, block_header.record_count, out);
    if (!ok) return false;

    cache_.block_index = block_index;
    cache_.records = out;
    return true;
}

Vbs4FrameData Vbs4File::read_frame(int frame_idx) const {
    Vbs4FrameData result;
    if (frame_idx < 0 || frame_idx >= static_cast<int>(summaries_.size())) return result;

    result.summary = summaries_[frame_idx];
    const auto& frame_idx_entry = frame_index_[result.summary.cu_index_entry];
    if (frame_idx_entry.block_index >= block_index_.size()) return result;
    const auto& block_idx_entry = block_index_[frame_idx_entry.block_index];
    if (frame_idx_entry.first_record < block_idx_entry.first_record) return result;
    const uint32_t local_first = frame_idx_entry.first_record - block_idx_entry.first_record;
    if (local_first > block_idx_entry.record_count ||
        frame_idx_entry.record_count > block_idx_entry.record_count - local_first) {
        return result;
    }

    std::vector<VbsCuRecord> block_records;
    if (!decode_block(frame_idx_entry.block_index, block_records)) return result;
    if (local_first + frame_idx_entry.record_count > block_records.size()) return result;

    result.cus.insert(result.cus.end(),
                      block_records.begin() + local_first,
                      block_records.begin() + local_first + frame_idx_entry.record_count);
    if (header_.codec == static_cast<uint16_t>(VbiCodec::H264)) {
        const uint32_t mb_width = std::max<uint32_t>((header_.width + 15) / 16, 1);
        for (uint32_t i = 0; i < result.cus.size(); ++i) {
            result.cus[i].common.x = clamp_u16((i % mb_width) * 16);
            result.cus[i].common.y = clamp_u16((i / mb_width) * 16);
        }
    }
    return result;
}

std::vector<Vbs4FrameSummary> Vbs4File::read_all_frame_summaries() const {
    return summaries_;
}

} // namespace vr::analysis
