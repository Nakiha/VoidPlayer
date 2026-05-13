#include "analysis/analysis_manager.h"
#include "common/win_utf8.h"

#include <algorithm>
#include <filesystem>
#include <limits>

namespace vr::analysis {

AnalysisManager& AnalysisManager::instance() {
    static AnalysisManager mgr;
    return mgr;
}

bool AnalysisManager::load(const std::string& analysis_path) {
    unload();
    return load_vac2(analysis_path);
}

bool AnalysisManager::load_vac2(const std::string& analysis_path) {
    if (!vac2_base_.open(analysis_path)) return false;
    analysis_path_ = analysis_path;
    loaded_ = true;
    {
        std::lock_guard<std::mutex> lock(overlay_chunk_index_mutex_);
        overlay_chunk_index_loaded_ = false;
        overlay_chunk_index_write_time_ = {};
        overlay_chunk_index_.clear();
    }
    return true;
}

void AnalysisManager::unload() {
    clear_overlay_tracks();
    vac2_base_.close();
    analysis_path_.clear();
    loaded_ = false;
    {
        std::lock_guard<std::mutex> lock(overlay_chunk_index_mutex_);
        overlay_chunk_index_loaded_ = false;
        overlay_chunk_index_write_time_ = {};
        overlay_chunk_index_.clear();
    }
    overlay.show_cu_grid.store(false, std::memory_order_release);
    overlay.show_pred_mode.store(false, std::memory_order_release);
    overlay.show_qp_heatmap.store(false, std::memory_order_release);
    overlay.show_pred_lines.store(false, std::memory_order_release);
    overlay.show_cu_bit_cost_heatmap.store(false, std::memory_order_release);
    overlay.opacity_permille.store(550, std::memory_order_release);
    overlay.mode.store(0, std::memory_order_release);
    overlay.track_file_id.store(-1, std::memory_order_release);
}

int AnalysisManager::frame_count() const {
    if (!loaded_) return 0;
    return static_cast<int>(vac2_base_.frames().size());
}

uint32_t AnalysisManager::video_width() const {
    if (!loaded_) return 0;
    return vac2_base_.header().width;
}

uint32_t AnalysisManager::video_height() const {
    if (!loaded_) return 0;
    return vac2_base_.header().height;
}

VachunkFrameSummary AnalysisManager::read_frame_summary(int frame_idx) const {
    if (!loaded_ || frame_idx < 0 || frame_idx >= frame_count()) return {};
    if (static_cast<size_t>(frame_idx) >= vac2_base_.frame_summaries().size()) return {};
    const auto& source = vac2_base_.frame_summaries()[static_cast<size_t>(frame_idx)];
    VachunkFrameSummary out{};
    out.poc = source.poc;
    out.coded_order = source.coded_order;
    out.vcl_unit_index = source.first_vcl_unit;
    out.flags = source.flags;
    out.temporal_id = source.temporal_id;
    out.slice_type = source.slice_type;
    out.nal_unit_type = source.nal_type;
    out.avg_qp = source.qp_avg;
    out.num_ref_l0 = source.num_ref_l0;
    out.num_ref_l1 = source.num_ref_l1;
    out.qp_min = source.qp_min;
    out.qp_max = source.qp_max;
    std::copy(std::begin(source.ref_pocs_l0), std::end(source.ref_pocs_l0),
              std::begin(out.ref_pocs_l0));
    std::copy(std::begin(source.ref_pocs_l1), std::end(source.ref_pocs_l1),
              std::begin(out.ref_pocs_l1));
    return out;
}

VachunkOverlayFrameData AnalysisManager::read_overlay_frame(int frame_idx) const {
    if (!loaded_ || frame_idx < 0 || frame_idx >= frame_count()) return {};
    return read_vac2_overlay_frame(frame_idx);
}

VachunkOverlayFrameData AnalysisManager::read_vac2_overlay_frame(int frame_idx) const {
    VachunkOverlayFrameData empty;
    if (analysis_path_.empty()) return empty;

    std::lock_guard<std::mutex> lock(overlay_chunk_index_mutex_);
    const auto base_path = win_utf8::path_from_utf8(analysis_path_);
    const auto overlay_dir = base_path.parent_path() / L"chunks" / L"overlay";
    std::error_code ec;
    const auto write_time = std::filesystem::last_write_time(overlay_dir, ec);
    const bool directory_changed =
        !ec && overlay_chunk_index_loaded_ &&
        write_time != overlay_chunk_index_write_time_;
    if (!overlay_chunk_index_loaded_ || directory_changed) {
        refresh_overlay_chunk_index_locked();
    }
    auto result = read_overlay_frame_from_index_locked(frame_idx);
    if (!result.cus.empty()) {
        return result;
    }
    refresh_overlay_chunk_index_locked();
    return read_overlay_frame_from_index_locked(frame_idx);
}

void AnalysisManager::refresh_overlay_chunk_index_locked() const {
    overlay_chunk_index_loaded_ = true;
    overlay_chunk_index_.clear();
    if (analysis_path_.empty()) return;

    const auto base_path = win_utf8::path_from_utf8(analysis_path_);
    const auto overlay_dir = base_path.parent_path() / L"chunks" / L"overlay";
    std::error_code ec;
    overlay_chunk_index_write_time_ = std::filesystem::last_write_time(overlay_dir, ec);
    if (ec || !std::filesystem::exists(overlay_dir, ec) || ec) return;

    for (const auto& entry : std::filesystem::directory_iterator(overlay_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec || entry.path().extension() != L".vck") {
            ec.clear();
            continue;
        }

        VachunkFile chunk;
        if (!chunk.open(win_utf8::path_to_utf8(entry.path()))) continue;
        const auto& header = chunk.header();
        if (header.kind != static_cast<uint16_t>(VachunkKind::Overlay)) {
            continue;
        }
        overlay_chunk_index_.push_back(OverlayChunkIndexEntry{
            header.start_frame,
            header.end_frame,
            header.base_content_revision,
            header.generator_revision,
            win_utf8::path_to_utf8(entry.path()),
        });
    }
}

VachunkOverlayFrameData AnalysisManager::read_overlay_frame_from_index_locked(int frame_idx) const {
    VachunkOverlayFrameData result;
    if (frame_idx < 0) return result;

    const OverlayChunkIndexEntry* best = nullptr;
    const auto target = static_cast<uint32_t>(frame_idx);
    for (const auto& entry : overlay_chunk_index_) {
        if (target < entry.start_frame || target > entry.end_frame) {
            continue;
        }
        if (!best ||
            entry.generator_revision > best->generator_revision ||
            (entry.generator_revision == best->generator_revision &&
             entry.base_revision > best->base_revision)) {
            best = &entry;
        }
    }

    if (!best) return result;

    VachunkFile chunk;
    if (!chunk.open(best->path)) return result;
    VachunkOverlayFrameData frame;
    if (!read_overlay_vachunk_frame(chunk, target, frame)) return result;
    result.summary = frame.summary;
    result.cus = std::move(frame.cus);
    return result;
}

bool AnalysisManager::set_overlay_track(int track_file_id, const std::string& analysis_path) {
    if (track_file_id < 0) return false;
    auto track_manager = std::make_shared<AnalysisManager>();
    if (!track_manager->load(analysis_path)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(overlay_tracks_mutex_);
    overlay_tracks_[track_file_id] = std::move(track_manager);
    return true;
}

void AnalysisManager::clear_overlay_tracks() {
    std::lock_guard<std::mutex> lock(overlay_tracks_mutex_);
    overlay_tracks_.clear();
}

std::vector<std::pair<int, std::shared_ptr<const AnalysisManager>>>
AnalysisManager::overlay_track_snapshot() const {
    std::lock_guard<std::mutex> lock(overlay_tracks_mutex_);
    std::vector<std::pair<int, std::shared_ptr<const AnalysisManager>>> tracks;
    tracks.reserve(overlay_tracks_.size());
    for (const auto& [track_file_id, manager] : overlay_tracks_) {
        tracks.emplace_back(track_file_id, manager);
    }
    return tracks;
}

int AnalysisManager::current_frame_idx(int64_t pts_us) const {
    if (!loaded_) return -1;
    const auto& h = vac2_base_.header();
    if (h.time_base_num == 0 || h.time_base_den == 0) return -1;
    const long double pts_tb =
        static_cast<long double>(pts_us) *
        static_cast<long double>(h.time_base_den) /
        (static_cast<long double>(h.time_base_num) * 1000000.0L);
    if (pts_tb < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        pts_tb > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
            return -1;
    }
    const int64_t target = static_cast<int64_t>(pts_tb);
    const auto& frames = vac2_base_.frames();
    auto it = std::upper_bound(
        frames.begin(),
        frames.end(),
        target,
        [](int64_t value, const Vac2FrameEntry& frame) {
            return value < frame.pts;
        });
    if (it == frames.begin()) return frames.empty() ? -1 : 0;
    --it;
    return static_cast<int>(std::distance(frames.begin(), it));
}

} // namespace vr::analysis
