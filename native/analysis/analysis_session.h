#pragma once

#include "analysis/cache/overlay_chunk.h"
#include "analysis/parsers/vac2_parser.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace vr::analysis {

class AnalysisSession {
public:
    bool open(const std::string& analysis_path);

    int frame_count() const;
    uint32_t video_width() const;
    uint32_t video_height() const;
    VachunkFrameSummary read_frame_summary(int frame_idx) const;
    VachunkOverlayFrameData read_overlay_frame(int frame_idx) const;
    int current_frame_idx(int64_t pts_us) const;
    int frame_idx_for_source_packet(int64_t packet_pos,
                                    int32_t packet_size,
                                    int32_t packet_index,
                                    int64_t packet_pts,
                                    int64_t packet_dts) const;
    void load_overlay_chunk_index() const;

    const Vac2BaseFile& vac2_base() const { return vac2_base_; }
    const std::string& analysis_path() const { return analysis_path_; }

private:
    struct OverlayChunkIndexEntry {
        uint32_t start_frame = 0;
        uint32_t end_frame = 0;
        uint64_t base_revision = 0;
        uint64_t generator_revision = 0;
        uint64_t feature_flags = 0;
        std::string path;
    };

    struct OverlayFrameCache {
        bool valid = false;
        uint32_t frame_index = 0;
        std::string chunk_path;
        std::filesystem::file_time_type write_time{};
        uint64_t file_size = 0;
        VachunkOverlayFrameData data;
    };

    struct OverlayDecodedChunkCacheEntry {
        std::string path;
        std::filesystem::file_time_type write_time{};
        uint64_t file_size = 0;
        uint64_t last_used = 0;
        DecodedOverlayChunk chunk;
    };

    VachunkOverlayFrameData read_vac2_overlay_frame(int frame_idx) const;
    void refresh_overlay_chunk_index_locked() const;
    VachunkOverlayFrameData read_overlay_frame_from_index_locked(int frame_idx) const;
    const OverlayDecodedChunkCacheEntry* decoded_overlay_chunk_for_path_locked(
        const std::string& path) const;

    Vac2BaseFile vac2_base_;
    std::string analysis_path_;

    mutable std::mutex overlay_chunk_index_mutex_;
    mutable bool overlay_chunk_index_loaded_ = false;
    mutable std::vector<OverlayChunkIndexEntry> overlay_chunk_index_;
    mutable OverlayFrameCache overlay_frame_cache_;
    mutable uint64_t overlay_decoded_chunk_cache_clock_ = 0;
    mutable std::vector<OverlayDecodedChunkCacheEntry> overlay_decoded_chunk_cache_;
};

} // namespace vr::analysis
