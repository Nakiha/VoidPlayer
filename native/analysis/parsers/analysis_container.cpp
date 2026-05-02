#include "analysis/parsers/analysis_container.h"
#include "common/win_utf8.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace vr::analysis {
namespace {

constexpr uint16_t kMaxContainerSections = 16;

bool fourcc_eq(const char lhs[4], const char rhs[4]) {
    return std::memcmp(lhs, rhs, 4) == 0;
}

bool range_fits(uint64_t offset, uint64_t size, uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

uint64_t file_size(const std::string& path) {
    if (path.empty()) return 0;
    std::ifstream f(win_utf8::path_from_utf8(path), std::ios::binary | std::ios::ate);
    if (!f) return 0;
    const auto size = f.tellg();
    return size < 0 ? 0 : static_cast<uint64_t>(size);
}

bool copy_file_bytes(std::ofstream& out, const std::string& path) {
    std::ifstream in(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!in) return false;

    std::vector<char> buffer(1024 * 1024);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = in.gcount();
        if (n > 0) {
            out.write(buffer.data(), n);
            if (!out) return false;
        }
    }
    return in.eof();
}

void set_fourcc(char out[4], const char type[4]) {
    std::memcpy(out, type, 4);
}

} // namespace

bool AnalysisContainerFile::open(const std::string& path) {
    close();

    std::ifstream f(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!f) return false;

    f.seekg(0, std::ios::end);
    const auto size_pos = f.tellg();
    if (size_pos < 0) return false;
    const uint64_t actual_size = static_cast<uint64_t>(size_pos);
    f.seekg(0, std::ios::beg);

    f.read(reinterpret_cast<char*>(&header_), sizeof(header_));
    if (!f ||
        header_.magic[0] != 'V' ||
        header_.magic[1] != 'A' ||
        header_.magic[2] != 'C' ||
        header_.magic[3] != '1' ||
        header_.version != 1 ||
        header_.header_size != sizeof(AnalysisContainerHeader) ||
        header_.section_entry_size != sizeof(AnalysisContainerSectionEntry) ||
        header_.section_count == 0 ||
        header_.section_count > kMaxContainerSections ||
        header_.file_size != actual_size ||
        !range_fits(header_.section_table_offset,
                    static_cast<uint64_t>(header_.section_count) * header_.section_entry_size,
                    actual_size)) {
        close();
        return false;
    }

    sections_.resize(header_.section_count);
    f.seekg(static_cast<std::streamoff>(header_.section_table_offset));
    f.read(reinterpret_cast<char*>(sections_.data()),
           static_cast<std::streamsize>(sections_.size() * sizeof(AnalysisContainerSectionEntry)));
    if (!f) {
        close();
        return false;
    }

    for (const auto& section : sections_) {
        if (section.size == 0 || !range_fits(section.offset, section.size, actual_size)) {
            close();
            return false;
        }
    }

    path_ = path;
    return true;
}

void AnalysisContainerFile::close() {
    path_.clear();
    header_ = {};
    sections_.clear();
}

const AnalysisContainerSectionEntry* AnalysisContainerFile::section(const char type[4]) const {
    for (const auto& section : sections_) {
        if (fourcc_eq(section.type, type)) return &section;
    }
    return nullptr;
}

bool write_analysis_container(const std::string& path,
                              const std::string& vbs3_path,
                              const std::string& vbi_path,
                              const std::string& vbt_path) {
    struct InputSection {
        const char* type;
        std::string path;
        uint64_t size;
    };

    std::vector<InputSection> inputs;
    const uint64_t vbs3_size = file_size(vbs3_path);
    if (vbs3_size > 0) inputs.push_back({"VBS3", vbs3_path, vbs3_size});

    const uint64_t vbi_size = file_size(vbi_path);
    const uint64_t vbt_size = file_size(vbt_path);
    if (vbi_size == 0 || vbt_size == 0) return false;
    inputs.push_back({"VBI2", vbi_path, vbi_size});
    inputs.push_back({"VBT1", vbt_path, vbt_size});

    std::ofstream out(win_utf8::path_from_utf8(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;

    AnalysisContainerHeader header{};
    header.magic[0] = 'V';
    header.magic[1] = 'A';
    header.magic[2] = 'C';
    header.magic[3] = '1';
    header.version = 1;
    header.header_size = sizeof(AnalysisContainerHeader);
    header.section_entry_size = sizeof(AnalysisContainerSectionEntry);
    header.section_count = static_cast<uint16_t>(inputs.size());
    header.section_table_offset = sizeof(AnalysisContainerHeader);

    std::vector<AnalysisContainerSectionEntry> sections(inputs.size());
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(sections.data()),
              static_cast<std::streamsize>(sections.size() * sizeof(AnalysisContainerSectionEntry)));
    if (!out) return false;

    for (size_t i = 0; i < inputs.size(); ++i) {
        auto& entry = sections[i];
        set_fourcc(entry.type, inputs[i].type);
        entry.offset = static_cast<uint64_t>(out.tellp());
        entry.size = inputs[i].size;
        if (!copy_file_bytes(out, inputs[i].path)) return false;
    }

    const auto end_pos = out.tellp();
    if (end_pos < 0) return false;
    header.file_size = static_cast<uint64_t>(end_pos);

    out.seekp(0, std::ios::beg);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(sections.data()),
              static_cast<std::streamsize>(sections.size() * sizeof(AnalysisContainerSectionEntry)));
    out.close();
    return !out.fail();
}

} // namespace vr::analysis
