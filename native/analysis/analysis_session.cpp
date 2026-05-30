#include "analysis/analysis_session.h"

#include "common/win_utf8.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace vr::analysis {
namespace {

constexpr size_t kDecodedOverlayChunkCacheLimit = 3;

struct FileFingerprint {
    bool valid = false;
    uint64_t file_size = 0;
    std::filesystem::file_time_type write_time{};
};

FileFingerprint read_file_fingerprint(const std::string& path) {
    FileFingerprint fingerprint;
    const auto fs_path = win_utf8::path_from_utf8(path);
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(fs_path, ec);
    if (ec) return fingerprint;
    const auto write_time = std::filesystem::last_write_time(fs_path, ec);
    if (ec) return fingerprint;
    fingerprint.valid = true;
    fingerprint.file_size = static_cast<uint64_t>(file_size);
    fingerprint.write_time = write_time;
    return fingerprint;
}

} // namespace

bool AnalysisSession::open(const std::string& analysis_path) {
    if (!vac2_base_.open(analysis_path)) return false;
    analysis_path_ = analysis_path;
    return true;
}

int AnalysisSession::frame_count() const {
    return static_cast<int>(vac2_base_.frames().size());
}

uint32_t AnalysisSession::video_width() const {
    return vac2_base_.header().width;
}

uint32_t AnalysisSession::video_height() const {
    return vac2_base_.header().height;
}

VachunkFrameSummary AnalysisSession::read_frame_summary(int frame_idx) const {
    if (frame_idx < 0) return {};
    const auto index = static_cast<size_t>(frame_idx);
    if (index >= vac2_base_.frame_summaries().size()) return {};
    const auto& source = vac2_base_.frame_summaries()[index];
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

VachunkOverlayFrameData AnalysisSession::read_overlay_frame(int frame_idx) const {
    if (frame_idx < 0 ||
        static_cast<size_t>(frame_idx) >= vac2_base_.frames().size()) {
        return {};
    }
    return read_vac2_overlay_frame(frame_idx);
}

VachunkOverlayFrameData AnalysisSession::read_vac2_overlay_frame(int frame_idx) const {
    VachunkOverlayFrameData empty;
    if (analysis_path_.empty()) return empty;

    std::lock_guard<std::mutex> lock(overlay_chunk_index_mutex_);
    if (!overlay_chunk_index_loaded_) {
        refresh_overlay_chunk_index_locked();
    }
    return read_overlay_frame_from_index_locked(frame_idx);
}

void AnalysisSession::load_overlay_chunk_index() const {
    std::lock_guard<std::mutex> lock(overlay_chunk_index_mutex_);
    refresh_overlay_chunk_index_locked();
}

void AnalysisSession::refresh_overlay_chunk_index_locked() const {
    overlay_chunk_index_loaded_ = true;
    overlay_chunk_index_.clear();
    overlay_frame_cache_ = {};
    overlay_decoded_chunk_cache_.clear();
    overlay_decoded_chunk_cache_clock_ = 0;
    if (analysis_path_.empty()) return;

    const auto base_path = win_utf8::path_from_utf8(analysis_path_);
    const auto overlay_dir = base_path.parent_path() / L"chunks" / L"overlay";
    std::error_code ec;
    if (!std::filesystem::exists(overlay_dir, ec) || ec) return;

    const auto& base_header = vac2_base_.header();
    constexpr uint64_t kRequiredOverlayFeatures = VACHUNK_FEATURE_CU_GEOMETRY;

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
        if (header.codec != base_header.codec ||
            header.track_index != base_header.track_index ||
            header.base_content_revision != base_header.content_revision ||
            (header.feature_flags & kRequiredOverlayFeatures) != kRequiredOverlayFeatures) {
            continue;
        }
        overlay_chunk_index_.push_back(OverlayChunkIndexEntry{
            header.start_frame,
            header.end_frame,
            header.base_content_revision,
            header.generator_revision,
            header.feature_flags,
            win_utf8::path_to_utf8(entry.path()),
        });
    }
}

VachunkOverlayFrameData AnalysisSession::read_overlay_frame_from_index_locked(
    int frame_idx) const {
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

    if (overlay_frame_cache_.valid &&
        overlay_frame_cache_.frame_index == target &&
        overlay_frame_cache_.chunk_path == best->path) {
        return overlay_frame_cache_.data;
    }

    const auto* decoded_entry =
        decoded_overlay_chunk_for_path_locked(best->path);
    if (!decoded_entry) return result;
    VachunkOverlayFrameData frame;
    if (!read_overlay_vachunk_frame(decoded_entry->chunk, target, frame)) return result;
    result.summary = frame.summary;
    result.cus = std::move(frame.cus);
    overlay_frame_cache_.valid = true;
    overlay_frame_cache_.frame_index = target;
    overlay_frame_cache_.chunk_path = best->path;
    overlay_frame_cache_.write_time = decoded_entry->write_time;
    overlay_frame_cache_.file_size = decoded_entry->file_size;
    overlay_frame_cache_.data = result;
    return result;
}

const AnalysisSession::OverlayDecodedChunkCacheEntry*
AnalysisSession::decoded_overlay_chunk_for_path_locked(const std::string& path) const {
    const uint64_t use_token = ++overlay_decoded_chunk_cache_clock_;
    auto& cache = overlay_decoded_chunk_cache_;
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->path != path) continue;
        it->last_used = use_token;
        return &(*it);
    }

    VachunkFile chunk;
    if (!chunk.open(path)) return nullptr;
    DecodedOverlayChunk decoded;
    if (!read_overlay_vachunk_chunk(chunk, decoded)) return nullptr;

    const auto fingerprint = read_file_fingerprint(path);

    if (cache.size() >= kDecodedOverlayChunkCacheLimit) {
        const auto oldest = std::min_element(
            cache.begin(),
            cache.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.last_used < rhs.last_used;
            });
        if (oldest != cache.end()) {
            cache.erase(oldest);
        }
    }
    cache.push_back(OverlayDecodedChunkCacheEntry{
        path,
        fingerprint.write_time,
        fingerprint.file_size,
        use_token,
        std::move(decoded),
    });
    return &cache.back();
}

int AnalysisSession::current_frame_idx(int64_t pts_us) const {
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
