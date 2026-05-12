#include "analysis/parsers/vac2_parser.h"
#include "common/win_utf8.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace vr::analysis {
namespace {

constexpr uint32_t kMaxVac2Sections = 64;
constexpr uint32_t kMaxVac2Records = 10'000'000;

bool fourcc_eq(const char lhs[4], const char rhs[4]) {
    return std::memcmp(lhs, rhs, 4) == 0;
}

void set_fourcc(char out[4], const char type[4]) {
    std::memcpy(out, type, 4);
}

bool range_fits(uint64_t offset, uint64_t size, uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

template <typename T>
bool read_record_section(std::ifstream& file,
                         const Vac2SectionEntry& section,
                         uint64_t file_size,
                         std::vector<T>& out) {
    out.clear();
    if (section.entry_size != sizeof(T) ||
        section.entry_count > kMaxVac2Records ||
        section.size != static_cast<uint64_t>(section.entry_size) * section.entry_count ||
        !range_fits(section.offset, section.size, file_size)) {
        return false;
    }
    out.resize(section.entry_count);
    if (out.empty()) return true;
    file.clear();
    file.seekg(static_cast<std::streamoff>(section.offset));
    if (!file) return false;
    file.read(reinterpret_cast<char*>(out.data()),
              static_cast<std::streamsize>(out.size() * sizeof(T)));
    return static_cast<size_t>(file.gcount()) == out.size() * sizeof(T);
}

bool read_string_section(std::ifstream& file,
                         const Vac2SectionEntry& section,
                         uint64_t file_size,
                         std::string& out) {
    out.clear();
    if (section.entry_size != 0 || section.entry_count != 0 ||
        section.size > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        !range_fits(section.offset, section.size, file_size)) {
        return false;
    }
    out.resize(static_cast<size_t>(section.size));
    if (out.empty()) return true;
    file.clear();
    file.seekg(static_cast<std::streamoff>(section.offset));
    if (!file) return false;
    file.read(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<size_t>(file.gcount()) == out.size();
}

void append_bytes(std::vector<uint8_t>& out, const void* data, size_t size) {
    if (size == 0) return;
    const auto* begin = static_cast<const uint8_t*>(data);
    out.insert(out.end(), begin, begin + size);
}

template <typename T>
void append_record_section(std::vector<uint8_t>& payload,
                           std::vector<Vac2SectionEntry>& sections,
                           const char type[4],
                           const std::vector<T>& records) {
    Vac2SectionEntry entry{};
    set_fourcc(entry.type, type);
    entry.offset = 0; // Patched after the section table size is known.
    entry.size = static_cast<uint64_t>(records.size()) * sizeof(T);
    entry.entry_size = sizeof(T);
    entry.entry_count = static_cast<uint32_t>(records.size());
    sections.push_back(entry);
    append_bytes(payload, records.data(), records.size() * sizeof(T));
}

void append_string_section(std::vector<uint8_t>& payload,
                           std::vector<Vac2SectionEntry>& sections,
                           const char type[4],
                           const std::string& text) {
    Vac2SectionEntry entry{};
    set_fourcc(entry.type, type);
    entry.offset = 0;
    entry.size = text.size();
    sections.push_back(entry);
    append_bytes(payload, text.data(), text.size());
}

bool validate_write_data(const Vac2BaseData& data) {
    return data.packets.size() <= kMaxVac2Records &&
           data.units.size() <= kMaxVac2Records &&
           data.frames.size() <= kMaxVac2Records &&
           data.frame_summaries.size() == data.frames.size();
}

} // namespace

bool Vac2BaseFile::open(const std::string& path) {
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
        header_.magic[1] != 'A' ||
        header_.magic[2] != 'C' ||
        header_.magic[3] != '2' ||
        header_.version_major != kVac2VersionMajor ||
        header_.header_size != sizeof(Vac2Header) ||
        header_.section_entry_size != sizeof(Vac2SectionEntry) ||
        header_.section_count == 0 ||
        header_.section_count > kMaxVac2Sections ||
        header_.file_size != actual_size ||
        header_.packet_count > kMaxVac2Records ||
        header_.unit_count > kMaxVac2Records ||
        header_.au_count > kMaxVac2Records ||
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
              static_cast<std::streamsize>(sections_.size() * sizeof(Vac2SectionEntry)));
    if (!file) {
        close();
        return false;
    }

    for (const auto& section_entry : sections_) {
        if (!range_fits(section_entry.offset, section_entry.size, actual_size)) {
            close();
            return false;
        }
    }

    const auto* meta = section("META");
    const auto* pkt2 = section("PKT2");
    const auto* bsu2 = section("BSU2");
    const auto* auf2 = section("AUF2");
    const auto* fsum = section("FSUM");
    if (!meta || !pkt2 || !bsu2 || !auf2 || !fsum) {
        close();
        return false;
    }

    if (!read_string_section(file, *meta, actual_size, metadata_json_) ||
        !read_record_section(file, *pkt2, actual_size, packets_) ||
        !read_record_section(file, *bsu2, actual_size, units_) ||
        !read_record_section(file, *auf2, actual_size, frames_) ||
        !read_record_section(file, *fsum, actual_size, frame_summaries_)) {
        close();
        return false;
    }

    if (packets_.size() != header_.packet_count ||
        units_.size() != header_.unit_count ||
        frames_.size() != header_.au_count ||
        frame_summaries_.size() != header_.au_count) {
        close();
        return false;
    }

    path_ = path;
    return true;
}

void Vac2BaseFile::close() {
    path_.clear();
    header_ = {};
    sections_.clear();
    metadata_json_.clear();
    packets_.clear();
    units_.clear();
    frames_.clear();
    frame_summaries_.clear();
}

const Vac2SectionEntry* Vac2BaseFile::section(const char type[4]) const {
    for (const auto& section_entry : sections_) {
        if (fourcc_eq(section_entry.type, type)) return &section_entry;
    }
    return nullptr;
}

bool write_vac2_base_container(const std::string& path,
                               const Vac2BaseData& data,
                               uint64_t max_output_bytes) {
    if (!validate_write_data(data)) return false;

    std::vector<Vac2SectionEntry> sections;
    std::vector<uint8_t> payload;
    sections.reserve(5);

    append_string_section(payload, sections, "META", data.metadata_json);
    append_record_section(payload, sections, "PKT2", data.packets);
    append_record_section(payload, sections, "BSU2", data.units);
    append_record_section(payload, sections, "AUF2", data.frames);
    append_record_section(payload, sections, "FSUM", data.frame_summaries);

    const uint64_t table_offset = sizeof(Vac2Header);
    const uint64_t table_size = static_cast<uint64_t>(sections.size()) * sizeof(Vac2SectionEntry);
    const uint64_t payload_offset = table_offset + table_size;
    const uint64_t expected_size = payload_offset + payload.size();
    if (max_output_bytes > 0 && expected_size > max_output_bytes) return false;

    uint64_t cursor = payload_offset;
    for (auto& section_entry : sections) {
        section_entry.offset = cursor;
        cursor += section_entry.size;
    }

    Vac2Header header{};
    header.magic[0] = 'V';
    header.magic[1] = 'A';
    header.magic[2] = 'C';
    header.magic[3] = '2';
    header.version_major = kVac2VersionMajor;
    header.version_minor = kVac2VersionMinor;
    header.header_size = sizeof(Vac2Header);
    header.section_entry_size = sizeof(Vac2SectionEntry);
    header.section_count = static_cast<uint32_t>(sections.size());
    header.codec = static_cast<uint16_t>(data.codec);
    header.track_index = data.track_index;
    header.time_base_num = data.time_base_num;
    header.time_base_den = data.time_base_den;
    header.packet_count = static_cast<uint32_t>(data.packets.size());
    header.unit_count = static_cast<uint32_t>(data.units.size());
    header.au_count = static_cast<uint32_t>(data.frames.size());
    header.width = data.width;
    header.height = data.height;
    header.section_table_offset = table_offset;
    header.file_size = expected_size;
    header.source_size = data.source_size;
    header.source_mtime_unix_ms = data.source_mtime_unix_ms;
    header.content_revision = data.content_revision;

    std::ofstream out(win_utf8::path_from_utf8(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(sections.data()),
              static_cast<std::streamsize>(sections.size() * sizeof(Vac2SectionEntry)));
    if (!payload.empty()) {
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
    }
    out.close();
    return !out.fail();
}

} // namespace vr::analysis
