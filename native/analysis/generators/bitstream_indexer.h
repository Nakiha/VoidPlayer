#pragma once

#include "analysis/parsers/binary_types.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vr::analysis {

struct BitstreamIndex {
    AnalysisCodec codec = AnalysisCodec::Unknown;
    AnalysisUnitKind unit_kind = AnalysisUnitKind::Unknown;
    std::vector<AnalysisUnitScanEntry> entries;
    uint64_t source_size = 0;
};

class BitstreamIndexer {
public:
    using AnalysisUnitScanEntrySink = std::function<bool(const AnalysisUnitScanEntry&)>;

    static AnalysisCodec codec_from_ffmpeg_id(int codec_id);
    static AnalysisCodec codec_from_path(const std::string& path);
    static AnalysisUnitKind unit_kind_for_codec(AnalysisCodec codec);

    static void append_packet(AnalysisCodec codec,
                              const uint8_t* data,
                              int data_len,
                              bool key_packet,
                              BitstreamIndex& index);

    static bool append_packet_streaming(AnalysisCodec codec,
                                        const uint8_t* data,
                                        int data_len,
                                        bool key_packet,
                                        uint64_t& source_size,
                                        const AnalysisUnitScanEntrySink& sink);

    static bool index_raw_file(const std::string& path,
                               AnalysisCodec codec,
                               BitstreamIndex& index);

    static bool index_raw_file_streaming(const std::string& path,
                                         AnalysisCodec codec,
                                         const AnalysisUnitScanEntrySink& sink,
                                         AnalysisCodec* resolved_codec = nullptr,
                                         uint64_t* source_size = nullptr);

    static bool write_annex_b_file(const std::string& path,
                                   AnalysisCodec codec,
                                   const std::string& output_path);
};

} // namespace vr::analysis
