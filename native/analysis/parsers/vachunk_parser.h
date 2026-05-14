#pragma once

#include "analysis/parsers/binary_types.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace vr::analysis {

struct VachunkPayloadSection {
    char type[4] = {};
    uint32_t flags = 0;
    uint32_t entry_size = 0;
    uint32_t entry_count = 0;
    uint64_t decoded_size = 0;
    std::vector<uint8_t> bytes;
};

struct VachunkData {
    VachunkKind kind = VachunkKind::Unknown;
    AnalysisCodec codec = AnalysisCodec::Unknown;
    uint64_t feature_flags = 0;
    uint64_t base_content_revision = 0;
    uint64_t generator_revision = 0;
    uint16_t track_index = 0;
    uint32_t start_frame = UINT32_MAX;
    uint32_t end_frame = UINT32_MAX;
    uint32_t start_packet = UINT32_MAX;
    uint32_t end_packet = UINT32_MAX;
    uint32_t start_unit = UINT32_MAX;
    uint32_t end_unit = UINT32_MAX;
    std::vector<VachunkPayloadSection> sections;
};

class VachunkFile {
public:
    bool open(const std::string& path);
    void close();

    const std::string& path() const { return path_; }
    const VachunkHeader& header() const { return header_; }
    const VachunkSectionEntry* section(const char type[4]) const;
    bool read_section(const char type[4], std::vector<uint8_t>& out) const;
    bool read_data(VachunkData& out) const;

private:
    std::string path_;
    VachunkHeader header_{};
    std::vector<VachunkSectionEntry> sections_;
};

inline constexpr bool vachunk_record_section_fits(size_t record_count,
                                                  size_t record_size) {
    return record_size <= std::numeric_limits<uint32_t>::max() &&
           record_count <= std::numeric_limits<uint32_t>::max() &&
           (record_size == 0 ||
            record_count <= std::numeric_limits<uint64_t>::max() / record_size);
}

template <typename T>
VachunkPayloadSection make_vachunk_record_section(const char (&type)[5],
                                                  const std::vector<T>& records,
                                                  uint32_t flags = 0) {
    VachunkPayloadSection section{};
    section.type[0] = type[0];
    section.type[1] = type[1];
    section.type[2] = type[2];
    section.type[3] = type[3];
    section.flags = flags;
    if (!vachunk_record_section_fits(records.size(), sizeof(T))) {
        section.entry_count = 1;
        return section;
    }
    section.entry_size = static_cast<uint32_t>(sizeof(T));
    section.entry_count = static_cast<uint32_t>(records.size());
    section.decoded_size = static_cast<uint64_t>(records.size()) * sizeof(T);
    if (!records.empty()) {
        const auto* begin = reinterpret_cast<const uint8_t*>(records.data());
        section.bytes.assign(begin, begin + section.decoded_size);
    }
    return section;
}

VachunkPayloadSection make_vachunk_bytes_section(const char (&type)[5],
                                                 const std::vector<uint8_t>& bytes,
                                                 uint32_t flags = 0);

VachunkPayloadSection make_vachunk_string_section(const char (&type)[5],
                                                  const std::string& text,
                                                  uint32_t flags = 0);

bool write_vachunk_file(const std::string& path,
                        const VachunkData& data,
                        uint64_t max_output_bytes = 0);

bool read_vachunk_file_data(const std::string& path, VachunkData& out);

} // namespace vr::analysis
