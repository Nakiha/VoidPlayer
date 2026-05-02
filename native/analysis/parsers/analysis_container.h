#pragma once

#include "analysis/parsers/binary_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vr::analysis {

class AnalysisContainerFile {
public:
    bool open(const std::string& path);
    void close();

    const std::string& path() const { return path_; }
    const AnalysisContainerHeader& header() const { return header_; }
    const AnalysisContainerSectionEntry* section(const char type[4]) const;

private:
    std::string path_;
    AnalysisContainerHeader header_{};
    std::vector<AnalysisContainerSectionEntry> sections_;
};

bool write_analysis_container(const std::string& path,
                              const std::string& vbs3_path,
                              const std::string& vbi_path,
                              const std::string& vbt_path);

} // namespace vr::analysis
