#include "analysis/parsers/vachunk_parser.h"
#include "common/win_utf8.h"

#include <cstring>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>
#include <zstd.h>

namespace vr::analysis {
namespace {

constexpr uint32_t kMaxVachunkSections = 128;
constexpr uint64_t kMaxVachunkSectionBytes = 1ull << 34; // 16 GiB guardrail.
constexpr size_t kZstdMinBenefitBytes = 16;
constexpr size_t kZstdMinSectionBytes = 1024;

bool fourcc_eq(const char lhs[4], const char rhs[4]) {
    return std::memcmp(lhs, rhs, 4) == 0;
}

bool range_fits(uint64_t offset, uint64_t size, uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

void set_fourcc(char out[4], const char type[4]) {
    std::memcpy(out, type, 4);
}

bool valid_scope(uint32_t start, uint32_t end) {
    return (start == UINT32_MAX && end == UINT32_MAX) ||
           (start != UINT32_MAX && end != UINT32_MAX && start <= end);
}

bool validate_write_data(const VachunkData& data) {
    if (data.sections.empty() || data.sections.size() > kMaxVachunkSections) {
        return false;
    }
    if (!is_defined_analysis_codec_value(static_cast<uint16_t>(data.codec))) {
        return false;
    }
    if (!valid_scope(data.start_frame, data.end_frame) ||
        !valid_scope(data.start_packet, data.end_packet) ||
        !valid_scope(data.start_unit, data.end_unit)) {
        return false;
    }
    for (const auto& section : data.sections) {
        if (section.bytes.size() > kMaxVachunkSectionBytes) return false;
        if (section.entry_size == 0 && section.entry_count != 0) return false;
        if (section.entry_size != 0) {
            const uint64_t expected =
                static_cast<uint64_t>(section.entry_size) * section.entry_count;
            if (expected != section.bytes.size()) return false;
        }
        if (section.decoded_size != 0 &&
            section.decoded_size != section.bytes.size()) {
            return false;
        }
    }
    return true;
}

struct EncodedSection {
    VachunkSectionEntry entry{};
    std::vector<uint8_t> bytes;
};

EncodedSection encode_section(const VachunkPayloadSection& source) {
    EncodedSection encoded;
    set_fourcc(encoded.entry.type, source.type);
    encoded.entry.flags = source.flags & ~VACHUNK_SECTION_FLAG_ZSTD;
    encoded.entry.entry_size = source.entry_size;
    encoded.entry.entry_count = source.entry_count;
    encoded.entry.decoded_size =
        source.decoded_size == 0 ? source.bytes.size() : source.decoded_size;
    encoded.bytes = source.bytes;

    if (source.bytes.size() >= kZstdMinSectionBytes) {
        const size_t bound = ZSTD_compressBound(source.bytes.size());
        std::vector<uint8_t> compressed(bound);
        const size_t compressed_size = ZSTD_compress(
            compressed.data(),
            compressed.size(),
            source.bytes.data(),
            source.bytes.size(),
            3);
        if (!ZSTD_isError(compressed_size) &&
            compressed_size + kZstdMinBenefitBytes < source.bytes.size()) {
            compressed.resize(compressed_size);
            encoded.bytes = std::move(compressed);
            encoded.entry.flags |= VACHUNK_SECTION_FLAG_ZSTD;
        }
    }

    encoded.entry.size = encoded.bytes.size();
    return encoded;
}

} // namespace

VachunkPayloadSection make_vachunk_bytes_section(const char (&type)[5],
                                                 const std::vector<uint8_t>& bytes,
                                                 uint32_t flags) {
    VachunkPayloadSection section{};
    section.type[0] = type[0];
    section.type[1] = type[1];
    section.type[2] = type[2];
    section.type[3] = type[3];
    section.flags = flags;
    section.decoded_size = bytes.size();
    section.bytes = bytes;
    return section;
}

VachunkPayloadSection make_vachunk_string_section(const char (&type)[5],
                                                  const std::string& text,
                                                  uint32_t flags) {
    const auto* begin = reinterpret_cast<const uint8_t*>(text.data());
    std::vector<uint8_t> bytes(begin, begin + text.size());
    return make_vachunk_bytes_section(type, bytes, flags);
}

bool VachunkFile::open(const std::string& path) {
    close();

    std::ifstream file(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!file) return false;

    file.seekg(0, std::ios::end);
    const auto size_pos = file.tellg();
    if (size_pos < 0) return false;
    const uint64_t actual_size = static_cast<uint64_t>(size_pos);
    file.seekg(0, std::ios::beg);

    file.read(reinterpret_cast<char*>(&header_), sizeof(header_));
    if (!file ||
        header_.magic[0] != 'V' ||
        header_.magic[1] != 'C' ||
        header_.magic[2] != 'K' ||
        header_.magic[3] != '1' ||
        header_.version_major != kVachunkVersionMajor ||
        header_.header_size != sizeof(VachunkHeader) ||
        header_.section_entry_size != sizeof(VachunkSectionEntry) ||
        header_.section_count == 0 ||
        header_.section_count > kMaxVachunkSections ||
        !is_defined_analysis_codec_value(header_.codec) ||
        (header_.compression != VACHUNK_COMPRESSION_NONE &&
         header_.compression != VACHUNK_COMPRESSION_ZSTD) ||
        header_.file_size != actual_size ||
        !valid_scope(header_.start_frame, header_.end_frame) ||
        !valid_scope(header_.start_packet, header_.end_packet) ||
        !valid_scope(header_.start_unit, header_.end_unit) ||
        !range_fits(header_.section_table_offset,
                    static_cast<uint64_t>(header_.section_count) *
                        header_.section_entry_size,
                    actual_size)) {
        close();
        return false;
    }

    sections_.resize(header_.section_count);
    file.seekg(static_cast<std::streamoff>(header_.section_table_offset));
    file.read(reinterpret_cast<char*>(sections_.data()),
              static_cast<std::streamsize>(sections_.size() * sizeof(VachunkSectionEntry)));
    if (!file) {
        close();
        return false;
    }

    for (const auto& section_entry : sections_) {
        const bool compressed =
            (section_entry.flags & VACHUNK_SECTION_FLAG_ZSTD) != 0;
        if (compressed && header_.compression != VACHUNK_COMPRESSION_ZSTD) {
            close();
            return false;
        }
        if (section_entry.size > kMaxVachunkSectionBytes ||
            section_entry.decoded_size > kMaxVachunkSectionBytes ||
            section_entry.decoded_size < section_entry.size ||
            !range_fits(section_entry.offset, section_entry.size, actual_size)) {
            close();
            return false;
        }
        if (!compressed && section_entry.decoded_size != section_entry.size) {
            close();
            return false;
        }
        if (section_entry.entry_size == 0 && section_entry.entry_count != 0) {
            close();
            return false;
        }
        if (section_entry.entry_size != 0 &&
            static_cast<uint64_t>(section_entry.entry_size) *
                section_entry.entry_count != section_entry.decoded_size) {
            close();
            return false;
        }
    }

    path_ = path;
    return true;
}

void VachunkFile::close() {
    path_.clear();
    header_ = {};
    sections_.clear();
}

const VachunkSectionEntry* VachunkFile::section(const char type[4]) const {
    for (const auto& section_entry : sections_) {
        if (fourcc_eq(section_entry.type, type)) return &section_entry;
    }
    return nullptr;
}

bool VachunkFile::read_section(const char type[4], std::vector<uint8_t>& out) const {
    out.clear();
    const auto* section_entry = section(type);
    if (!section_entry || path_.empty()) return false;
    if (section_entry->size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        section_entry->decoded_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    std::ifstream file(win_utf8::path_from_utf8(path_), std::ios::binary);
    if (!file) return false;
    std::vector<uint8_t> encoded(static_cast<size_t>(section_entry->size));
    if (encoded.empty()) return true;
    file.seekg(static_cast<std::streamoff>(section_entry->offset));
    file.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    if (static_cast<size_t>(file.gcount()) != encoded.size()) return false;

    if ((section_entry->flags & VACHUNK_SECTION_FLAG_ZSTD) == 0) {
        out = std::move(encoded);
        return true;
    }

    out.resize(static_cast<size_t>(section_entry->decoded_size));
    const size_t decoded = ZSTD_decompress(
        out.data(),
        out.size(),
        encoded.data(),
        encoded.size());
    return !ZSTD_isError(decoded) && decoded == out.size();
}

bool VachunkFile::read_data(VachunkData& out) const {
    out = {};
    if (path_.empty()) return false;
    out.kind = static_cast<VachunkKind>(header_.kind);
    out.codec = analysis_codec_from_u16(header_.codec);
    out.feature_flags = header_.feature_flags;
    out.base_content_revision = header_.base_content_revision;
    out.generator_revision = header_.generator_revision;
    out.track_index = header_.track_index;
    out.start_frame = header_.start_frame;
    out.end_frame = header_.end_frame;
    out.start_packet = header_.start_packet;
    out.end_packet = header_.end_packet;
    out.start_unit = header_.start_unit;
    out.end_unit = header_.end_unit;

    out.sections.reserve(sections_.size());
    for (const auto& entry : sections_) {
        VachunkPayloadSection section{};
        set_fourcc(section.type, entry.type);
        section.flags = entry.flags & ~VACHUNK_SECTION_FLAG_ZSTD;
        section.entry_size = entry.entry_size;
        section.entry_count = entry.entry_count;
        section.decoded_size = entry.decoded_size;
        if (!read_section(entry.type, section.bytes)) {
            out = {};
            return false;
        }
        if (section.decoded_size != section.bytes.size()) {
            out = {};
            return false;
        }
        out.sections.push_back(std::move(section));
    }
    return true;
}

bool write_vachunk_file(const std::string& path,
                        const VachunkData& data,
                        uint64_t max_output_bytes) {
    if (!validate_write_data(data)) {
        spdlog::error("[VACHUNK] invalid write data: path={}, sections={}, kind={}, codec={}, frames={}-{} packets={}-{} units={}-{}",
                      path,
                      data.sections.size(),
                      static_cast<int>(data.kind),
                      static_cast<int>(data.codec),
                      data.start_frame,
                      data.end_frame,
                      data.start_packet,
                      data.end_packet,
                      data.start_unit,
                      data.end_unit);
        return false;
    }

    std::vector<EncodedSection> encoded_sections;
    encoded_sections.reserve(data.sections.size());
    for (const auto& section : data.sections) {
        encoded_sections.push_back(encode_section(section));
    }

    const uint64_t table_offset = sizeof(VachunkHeader);
    const uint64_t table_size =
        static_cast<uint64_t>(encoded_sections.size()) * sizeof(VachunkSectionEntry);
    const uint64_t payload_offset = table_offset + table_size;
    uint64_t expected_size = payload_offset;
    for (const auto& section : encoded_sections) {
        if (section.bytes.size() > UINT64_MAX - expected_size) return false;
        expected_size += section.bytes.size();
    }
    if (max_output_bytes > 0 && expected_size > max_output_bytes) {
        spdlog::error("[VACHUNK] output exceeds byte limit: path={}, expected={}, max={}",
                      path, expected_size, max_output_bytes);
        return false;
    }

    bool uses_zstd = false;
    for (const auto& section : encoded_sections) {
        if ((section.entry.flags & VACHUNK_SECTION_FLAG_ZSTD) != 0) {
            uses_zstd = true;
            break;
        }
    }

    VachunkHeader header{};
    header.magic[0] = 'V';
    header.magic[1] = 'C';
    header.magic[2] = 'K';
    header.magic[3] = '1';
    header.version_major = kVachunkVersionMajor;
    header.version_minor = kVachunkVersionMinor;
    header.header_size = sizeof(VachunkHeader);
    header.section_entry_size = sizeof(VachunkSectionEntry);
    header.section_count = static_cast<uint32_t>(data.sections.size());
    header.kind = static_cast<uint16_t>(data.kind);
    header.codec = static_cast<uint16_t>(data.codec);
    header.feature_flags = data.feature_flags;
    header.base_content_revision = data.base_content_revision;
    header.generator_revision = data.generator_revision;
    header.track_index = data.track_index;
    header.compression = uses_zstd ? VACHUNK_COMPRESSION_ZSTD
                                   : VACHUNK_COMPRESSION_NONE;
    header.start_frame = data.start_frame;
    header.end_frame = data.end_frame;
    header.start_packet = data.start_packet;
    header.end_packet = data.end_packet;
    header.start_unit = data.start_unit;
    header.end_unit = data.end_unit;
    header.section_table_offset = table_offset;
    header.file_size = expected_size;

    uint64_t cursor = payload_offset;
    for (auto& section : encoded_sections) {
        auto& entry = section.entry;
        entry.offset = cursor;
        cursor += entry.size;
    }

    std::ofstream out(win_utf8::path_from_utf8(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        spdlog::error("[VACHUNK] failed to open output: {}", path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    for (const auto& section : encoded_sections) {
        out.write(reinterpret_cast<const char*>(&section.entry),
                  static_cast<std::streamsize>(sizeof(VachunkSectionEntry)));
    }
    for (const auto& section : encoded_sections) {
        if (!section.bytes.empty()) {
            out.write(reinterpret_cast<const char*>(section.bytes.data()),
                      static_cast<std::streamsize>(section.bytes.size()));
        }
    }
    out.close();
    if (out.fail()) {
        spdlog::error("[VACHUNK] failed while writing output: {}", path);
        return false;
    }
    return true;
}

bool read_vachunk_file_data(const std::string& path, VachunkData& out) {
    VachunkFile file;
    if (!file.open(path)) return false;
    return file.read_data(out);
}

} // namespace vr::analysis
