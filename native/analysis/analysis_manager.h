#pragma once

#include "analysis/cache/overlay_chunk.h"
#include "analysis/parsers/vac2_parser.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vr::analysis {

class AnalysisManager {
public:
    AnalysisManager() = default;
    static AnalysisManager& instance();

    bool load(const std::string& analysis_path);
    void unload();
    bool is_loaded() const;

    int frame_count() const;
    uint32_t video_width() const;
    uint32_t video_height() const;
    VachunkFrameSummary read_frame_summary(int frame_idx) const;
    VachunkOverlayFrameData read_overlay_frame(int frame_idx) const;

    // Overlay state (written by Dart via FFI, read by render thread)
    struct OverlayState {
        std::atomic<bool> show_cu_grid{false};
        std::atomic<bool> show_pred_mode{false};
        std::atomic<bool> show_qp_heatmap{false};
        std::atomic<bool> show_pred_lines{false};
        std::atomic<bool> show_cu_bit_cost_heatmap{false};
        std::atomic<int> opacity_permille{550};
        std::atomic<int> mode{0};
        std::atomic<int> track_file_id{-1};
    };

    OverlayState overlay;
    const OverlayState& overlay_state() const { return overlay; }

    bool set_overlay_track(int track_file_id, const std::string& analysis_path);
    void clear_overlay_tracks();
    std::vector<std::pair<int, std::shared_ptr<const AnalysisManager>>>
    overlay_track_snapshot() const;

    // Derive current frame index from PTS
    int current_frame_idx(int64_t pts_us) const;

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

    struct Session {
        Vac2BaseFile vac2_base;
        std::string analysis_path;
        mutable std::mutex overlay_chunk_index_mutex;
        mutable bool overlay_chunk_index_loaded = false;
        mutable std::filesystem::file_time_type overlay_chunk_index_write_time{};
        mutable std::vector<OverlayChunkIndexEntry> overlay_chunk_index;
        mutable OverlayFrameCache overlay_frame_cache;
        mutable uint64_t overlay_decoded_chunk_cache_clock = 0;
        mutable std::vector<OverlayDecodedChunkCacheEntry> overlay_decoded_chunk_cache;
    };

    std::shared_ptr<const Session> session_snapshot() const;
    bool load_vac2(const std::string& analysis_path);
    VachunkOverlayFrameData read_vac2_overlay_frame(
        const std::shared_ptr<const Session>& session,
        int frame_idx) const;
    void refresh_overlay_chunk_index_locked(const Session& session) const;
    VachunkOverlayFrameData read_overlay_frame_from_index_locked(
        const Session& session,
        int frame_idx) const;
    const OverlayDecodedChunkCacheEntry* decoded_overlay_chunk_for_path_locked(
        const Session& session,
        const std::string& path) const;

    mutable std::mutex session_mutex_;
    std::shared_ptr<const Session> session_;
    mutable std::mutex overlay_tracks_mutex_;
    std::unordered_map<int, std::shared_ptr<AnalysisManager>> overlay_tracks_;
};

} // namespace vr::analysis
