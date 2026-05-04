#include "analysis/parsers/vbs3_parser.h"
#include "common/win_utf8.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstring>
#include <limits>

namespace vr::analysis {

namespace {
constexpr uint32_t kMaxFrames = 10'000'000;
constexpr uint32_t kMaxCusPerFrame = 2'000'000;
constexpr uint32_t kMaxSections = 128;

bool fourcc_eq(const char lhs[4], const char rhs[4]) {
    return std::memcmp(lhs, rhs, 4) == 0;
}

bool range_fits(uint64_t offset, uint64_t size, uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

bool section_records_fit(const Vbs3SectionEntry& section, uint32_t expected_entry_size) {
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

#ifdef _WIN32
constexpr DWORD kVoidCompressAlgorithmXpressHuff = 4;

using VoidDecompressorHandle = void*;
using VoidCreateDecompressorProc = BOOL(WINAPI*)(DWORD, void*, VoidDecompressorHandle*);
using VoidDecompressProc = BOOL(WINAPI*)(VoidDecompressorHandle, void*, size_t, void*, size_t, size_t*);
using VoidCloseDecompressorProc = BOOL(WINAPI*)(VoidDecompressorHandle);

struct VoidDecompressionApi {
    bool initialized = false;
    bool available = false;
    HMODULE module = nullptr;
    VoidCreateDecompressorProc create_decompressor = nullptr;
    VoidDecompressProc decompress = nullptr;
    VoidCloseDecompressorProc close_decompressor = nullptr;
};

VoidDecompressionApi& decompression_api() {
    static VoidDecompressionApi api;
    if (!api.initialized) {
        api.initialized = true;
        api.module = LoadLibraryA("Cabinet.dll");
        if (api.module) {
            api.create_decompressor = reinterpret_cast<VoidCreateDecompressorProc>(
                GetProcAddress(api.module, "CreateDecompressor"));
            api.decompress = reinterpret_cast<VoidDecompressProc>(
                GetProcAddress(api.module, "Decompress"));
            api.close_decompressor = reinterpret_cast<VoidCloseDecompressorProc>(
                GetProcAddress(api.module, "CloseDecompressor"));
            api.available = api.create_decompressor && api.decompress && api.close_decompressor;
        }
    }
    return api;
}

bool decompress_xpress_huff(const std::vector<uint8_t>& input,
                            size_t max_output_size,
                            std::vector<uint8_t>& output) {
    auto& api = decompression_api();
    if (!api.available || input.empty() || max_output_size == 0) return false;

    VoidDecompressorHandle decompressor = nullptr;
    if (!api.create_decompressor(kVoidCompressAlgorithmXpressHuff, nullptr, &decompressor)) {
        return false;
    }

    output.resize(max_output_size);
    size_t output_size = 0;
    const BOOL ok = api.decompress(decompressor,
                                   const_cast<uint8_t*>(input.data()),
                                   input.size(),
                                   output.data(),
                                   output.size(),
                                   &output_size);
    api.close_decompressor(decompressor);
    if (!ok || output_size > output.size()) {
        output.clear();
        return false;
    }
    output.resize(static_cast<size_t>(output_size));
    return true;
}
#else
bool decompress_xpress_huff(const std::vector<uint8_t>&,
                            size_t,
                            std::vector<uint8_t>&) {
    return false;
}
#endif

bool parse_cu_payload(const uint8_t* data,
                      size_t size,
                      uint32_t cu_count,
                      std::vector<VbsCuRecord>& out) {
    if (cu_count == 0) return true;
    if (!data) return false;

    const uint8_t* cursor = data;
    const uint8_t* end = data + size;

    out.reserve(cu_count);
    for (uint32_t i = 0; i < cu_count; ++i) {
        if (static_cast<size_t>(end - cursor) < sizeof(VbsCuCommon)) {
            return false;
        }

        VbsCuRecord rec{};
        std::memcpy(&rec.common, cursor, sizeof(VbsCuCommon));
        cursor += sizeof(VbsCuCommon);

        switch (rec.common.pred_mode) {
        case 1:
            if (static_cast<size_t>(end - cursor) < sizeof(VbsCuIntra)) return false;
            std::memcpy(&rec.intra, cursor, sizeof(VbsCuIntra));
            cursor += sizeof(VbsCuIntra);
            break;
        case 0:
            if (static_cast<size_t>(end - cursor) < sizeof(VbsCuInter)) return false;
            std::memcpy(&rec.inter, cursor, sizeof(VbsCuInter));
            cursor += sizeof(VbsCuInter);
            break;
        default:
            break;
        }

        out.push_back(rec);
    }
    return true;
}

bool validate_summary(const Vbs3FrameSummary& summary, uint32_t index, size_t cu_index_count) {
    if (summary.coded_order != index) return false;
    if (summary.num_ref_l0 > 15 || summary.num_ref_l1 > 15) return false;
    if (summary.num_cus > kMaxCusPerFrame) return false;
    if (summary.cu_index_entry >= cu_index_count) return false;
    if (summary.avg_qp > 63 || summary.qp_min > 63 || summary.qp_max > 63) return false;
    if (summary.num_cus > 0 && summary.qp_min > summary.qp_max) return false;
    return true;
}

bool validate_cu_index(const Vbs3CuIndexEntry& idx, uint64_t cu_blob_size) {
    constexpr uint32_t kKnownFlags = VBS3_CUID_FLAG_COMPRESSED_XPRESS_HUFF;
    const bool compressed = (idx.flags & VBS3_CUID_FLAG_COMPRESSED_XPRESS_HUFF) != 0;
    if ((idx.flags & ~kKnownFlags) != 0) return false;
    if (idx.cu_count > kMaxCusPerFrame) return false;
    if (idx.offset > cu_blob_size) return false;
    if (idx.byte_size > cu_blob_size - idx.offset) return false;
    if (compressed) return idx.byte_size > 0 || idx.cu_count == 0;
    return idx.byte_size >= static_cast<uint64_t>(idx.cu_count) * sizeof(VbsCuCommon);
}
} // namespace

bool Vbs3File::open(const std::string& path) {
    close();
    std::ifstream f(win_utf8::path_from_utf8(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto size = f.tellg();
    if (size < 0) return false;
    f.close();
    return open_region(path, 0, static_cast<uint64_t>(size));
}

bool Vbs3File::open_region(const std::string& path, uint64_t offset, uint64_t size) {
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

    file_.read(reinterpret_cast<char*>(&header_), sizeof(Vbs3Header));
    if (!file_ || header_.magic[0] != 'V' || header_.magic[1] != 'B' ||
                 header_.magic[2] != 'S' || header_.magic[3] != '3') {
        close();
        return false;
    }
    if (header_.version_major != 3 ||
        header_.header_size != sizeof(Vbs3Header) ||
        header_.section_entry_size != sizeof(Vbs3SectionEntry) ||
        header_.frame_count > kMaxFrames ||
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
               static_cast<std::streamsize>(sections_.size() * sizeof(Vbs3SectionEntry)));
    if (!file_) {
        close();
        return false;
    }

    const auto* fsum = section("FSUM");
    const auto* cuid = section("CUID");
    const auto* cubl = section("CUBL");
    if (!fsum || !cuid || !cubl ||
        !section_records_fit(*fsum, sizeof(Vbs3FrameSummary)) ||
        !section_records_fit(*cuid, sizeof(Vbs3CuIndexEntry)) ||
        fsum->entry_count != header_.frame_count ||
        cuid->entry_count != header_.frame_count ||
        cubl->entry_count != header_.frame_count ||
        cubl->entry_size != 0 ||
        !range_fits(fsum->offset, fsum->size, file_size) ||
        !range_fits(cuid->offset, cuid->size, file_size) ||
        !range_fits(cubl->offset, cubl->size, file_size)) {
        close();
        return false;
    }

    summaries_.resize(fsum->entry_count);
    file_.seekg(static_cast<std::streamoff>(base_offset_ + fsum->offset));
    if (!summaries_.empty()) {
        file_.read(reinterpret_cast<char*>(summaries_.data()),
                   static_cast<std::streamsize>(summaries_.size() * sizeof(Vbs3FrameSummary)));
    }

    cu_index_.resize(cuid->entry_count);
    file_.seekg(static_cast<std::streamoff>(base_offset_ + cuid->offset));
    if (!cu_index_.empty()) {
        file_.read(reinterpret_cast<char*>(cu_index_.data()),
                   static_cast<std::streamsize>(cu_index_.size() * sizeof(Vbs3CuIndexEntry)));
    }
    if (!file_) {
        close();
        return false;
    }

    cu_blob_offset_ = cubl->offset;
    cu_blob_size_ = cubl->size;

    for (size_t i = 0; i < summaries_.size(); ++i) {
        const auto& summary = summaries_[i];
        if (!validate_summary(summary, static_cast<uint32_t>(i), cu_index_.size())) {
            close();
            return false;
        }
        const auto& idx = cu_index_[summary.cu_index_entry];
        if (idx.cu_count != summary.num_cus ||
            !validate_cu_index(idx, cu_blob_size_)) {
            close();
            return false;
        }
    }

    return true;
}

void Vbs3File::close() {
    file_.close();
    base_offset_ = 0;
    region_size_ = 0;
    sections_.clear();
    summaries_.clear();
    cu_index_.clear();
    header_ = {};
    cu_blob_offset_ = 0;
    cu_blob_size_ = 0;
}

const Vbs3SectionEntry* Vbs3File::section(const char type[4]) const {
    for (const auto& entry : sections_) {
        if (fourcc_eq(entry.type, type)) return &entry;
    }
    return nullptr;
}

Vbs3FrameSummary Vbs3File::read_frame_summary(int frame_idx) const {
    if (frame_idx < 0 || frame_idx >= static_cast<int>(summaries_.size())) {
        return {};
    }
    return summaries_[frame_idx];
}

Vbs3FrameData Vbs3File::read_frame(int frame_idx) const {
    Vbs3FrameData result;
    if (frame_idx < 0 || frame_idx >= static_cast<int>(summaries_.size())) return result;

    result.summary = summaries_[frame_idx];
    const auto& idx = cu_index_[result.summary.cu_index_entry];
    if (idx.cu_count > kMaxCusPerFrame ||
        idx.offset > cu_blob_size_ ||
        idx.byte_size > cu_blob_size_ - idx.offset) {
        return result;
    }

    std::vector<uint8_t> stored;
    if (!read_bytes(file_, base_offset_ + cu_blob_offset_ + idx.offset, idx.byte_size, stored)) {
        return result;
    }

    const bool compressed = (idx.flags & VBS3_CUID_FLAG_COMPRESSED_XPRESS_HUFF) != 0;
    std::vector<uint8_t> payload;
    const uint8_t* payload_data = stored.data();
    size_t payload_size = stored.size();
    if (compressed) {
        if (idx.cu_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) /
                               VBS_CU_SIZE_INTER) {
            return result;
        }
        const size_t max_payload_size = static_cast<size_t>(idx.cu_count) * VBS_CU_SIZE_INTER;
        if (!decompress_xpress_huff(stored, max_payload_size, payload)) {
            return result;
        }
        payload_data = payload.data();
        payload_size = payload.size();
    }

    parse_cu_payload(payload_data, payload_size, idx.cu_count, result.cus);
    return result;
}

std::vector<Vbs3FrameSummary> Vbs3File::read_all_frame_summaries() const {
    return summaries_;
}

} // namespace vr::analysis
