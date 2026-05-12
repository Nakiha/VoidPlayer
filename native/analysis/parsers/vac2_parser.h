#pragma once

#include "analysis/parsers/binary_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vr::analysis {

struct Vac2BaseData {
    VbiCodec codec = VbiCodec::Unknown;
    uint16_t track_index = 0;
    int32_t time_base_num = 0;
    int32_t time_base_den = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t source_size = 0;
    int64_t source_mtime_unix_ms = 0;
    uint64_t content_revision = 0;
    std::string metadata_json;
    std::vector<Vac2PacketEntry> packets;
    std::vector<Vac2BitstreamUnitEntry> units;
    std::vector<Vac2FrameEntry> frames;
    std::vector<Vac2FrameSummaryEntry> frame_summaries;
};

class Vac2BaseFile {
public:
    bool open(const std::string& path);
    void close();

    const std::string& path() const { return path_; }
    const Vac2Header& header() const { return header_; }
    const Vac2SectionEntry* section(const char type[4]) const;

    const std::string& metadata_json() const { return metadata_json_; }
    const std::vector<Vac2PacketEntry>& packets() const { return packets_; }
    const std::vector<Vac2BitstreamUnitEntry>& units() const { return units_; }
    const std::vector<Vac2FrameEntry>& frames() const { return frames_; }
    const std::vector<Vac2FrameSummaryEntry>& frame_summaries() const {
        return frame_summaries_;
    }

private:
    std::string path_;
    Vac2Header header_{};
    std::vector<Vac2SectionEntry> sections_;
    std::string metadata_json_;
    std::vector<Vac2PacketEntry> packets_;
    std::vector<Vac2BitstreamUnitEntry> units_;
    std::vector<Vac2FrameEntry> frames_;
    std::vector<Vac2FrameSummaryEntry> frame_summaries_;
};

bool write_vac2_base_container(const std::string& path,
                               const Vac2BaseData& data,
                               uint64_t max_output_bytes = 0);

} // namespace vr::analysis
