#pragma once

#include "analysis/parsers/binary_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vr::analysis {

struct BitstreamIndex {
    VbiCodec codec = VbiCodec::Unknown;
    VbiUnitKind unit_kind = VbiUnitKind::Unknown;
    std::vector<VbiEntry> entries;
    uint64_t source_size = 0;
};

class BitstreamIndexer {
public:
    static VbiCodec codec_from_ffmpeg_id(int codec_id);
    static VbiCodec codec_from_path(const std::string& path);
    static VbiUnitKind unit_kind_for_codec(VbiCodec codec);

    static void append_packet(VbiCodec codec,
                              const uint8_t* data,
                              int data_len,
                              bool key_packet,
                              BitstreamIndex& index);

    static bool index_raw_file(const std::string& path,
                               VbiCodec codec,
                               BitstreamIndex& index);
};

} // namespace vr::analysis
