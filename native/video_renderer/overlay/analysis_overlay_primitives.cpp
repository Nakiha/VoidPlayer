#include "video_renderer/overlay/analysis_overlay_primitives.h"

#include "analysis/analysis_manager.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace vr {
namespace {

constexpr int kAnalysisOverlayHeatmapNone = 0;
constexpr int kAnalysisOverlayHeatmapQp = 1;
constexpr int kAnalysisOverlayHeatmapBitCost = 2;

uint8_t overlay_fill_alpha(int opacity_permille, bool heatmap) {
    const int base = std::clamp(opacity_permille, 0, 1000) * 255 / 1000;
    return static_cast<uint8_t>(heatmap ? base : base * 2 / 5);
}

int find_track_slot(const RendererDrawSnapshot& snapshot, int track_file_id) {
    for (size_t i = 0; i < snapshot.tracks.size(); ++i) {
        if (snapshot.tracks[i].active && snapshot.tracks[i].file_id == track_file_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct OverlayPrimitiveTrackCacheKey {
    const void* session = nullptr;
    int track_file_id = -1;
    int slot = -1;
    int frame_index = -1;
    int video_width = 0;
    int video_height = 0;
    int mode = -1;
    int opacity_permille = -1;
    bool show_grid = false;
    bool show_qp = false;
    bool show_pred = false;
    bool show_lines = false;
    bool show_bit_cost = false;

    bool operator==(const OverlayPrimitiveTrackCacheKey& other) const {
        return session == other.session &&
               track_file_id == other.track_file_id &&
               slot == other.slot &&
               frame_index == other.frame_index &&
               video_width == other.video_width &&
               video_height == other.video_height &&
               mode == other.mode &&
               opacity_permille == other.opacity_permille &&
               show_grid == other.show_grid &&
               show_qp == other.show_qp &&
               show_pred == other.show_pred &&
               show_lines == other.show_lines &&
               show_bit_cost == other.show_bit_cost;
    }
};

struct OverlayPrimitivePackageCacheKey {
    std::vector<OverlayPrimitiveTrackCacheKey> tracks;

    bool operator==(const OverlayPrimitivePackageCacheKey& other) const {
        return tracks == other.tracks;
    }
};

struct OverlayPrimitiveBuildSource {
    OverlayPrimitiveTrackCacheKey key;
    std::shared_ptr<const analysis::AnalysisSession> analysis;
};

struct OverlayPrimitivePackageCacheEntry {
    OverlayPrimitivePackageCacheKey key;
    std::shared_ptr<const AnalysisOverlayPrimitivePackage> package;
    uint64_t last_used = 0;
};

constexpr size_t kOverlayPrimitivePackageCacheLimit = 24;

std::mutex& overlay_primitive_cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<OverlayPrimitivePackageCacheEntry>& overlay_primitive_cache_entries() {
    static std::vector<OverlayPrimitivePackageCacheEntry> entries;
    return entries;
}

uint64_t& overlay_primitive_cache_clock() {
    static uint64_t clock = 0;
    return clock;
}

uint64_t next_overlay_primitive_package_generation() {
    static std::atomic<uint64_t> generation{1};
    return generation.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const AnalysisOverlayPrimitivePackage> empty_overlay_primitive_package() {
    static const auto empty = std::make_shared<const AnalysisOverlayPrimitivePackage>();
    return empty;
}

std::shared_ptr<const AnalysisOverlayPrimitivePackage> lookup_overlay_primitive_package(
    const OverlayPrimitivePackageCacheKey& key) {
    std::lock_guard<std::mutex> lock(overlay_primitive_cache_mutex());
    auto& clock = overlay_primitive_cache_clock();
    const uint64_t use_token = ++clock;
    for (auto& entry : overlay_primitive_cache_entries()) {
        if (entry.key == key) {
            entry.last_used = use_token;
            return entry.package;
        }
    }
    return nullptr;
}

void store_overlay_primitive_package(
    OverlayPrimitivePackageCacheKey key,
    std::shared_ptr<const AnalysisOverlayPrimitivePackage> package) {
    if (!package) {
        return;
    }
    std::lock_guard<std::mutex> lock(overlay_primitive_cache_mutex());
    auto& clock = overlay_primitive_cache_clock();
    auto& entries = overlay_primitive_cache_entries();
    const uint64_t use_token = ++clock;
    for (auto& entry : entries) {
        if (entry.key == key) {
            entry.package = std::move(package);
            entry.last_used = use_token;
            return;
        }
    }
    if (entries.size() >= kOverlayPrimitivePackageCacheLimit) {
        const auto oldest = std::min_element(
            entries.begin(),
            entries.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.last_used < rhs.last_used;
            });
        if (oldest != entries.end()) {
            entries.erase(oldest);
        }
    }
    entries.push_back(
        OverlayPrimitivePackageCacheEntry{std::move(key), std::move(package), use_token});
}

AnalysisOverlayTrackPrimitives build_track_primitives(
    const OverlayPrimitiveTrackCacheKey& key,
    const analysis::VachunkOverlayFrameData& frame,
    bool qp_primary,
    bool bit_cost_primary,
    bool pred_primary,
    bool needs_outlines,
    uint8_t fill_alpha,
    uint8_t line_alpha) {
    AnalysisOverlayTrackPrimitives track;
    track.slot = key.slot;
    track.track_file_id = key.track_file_id;
    track.frame_index = key.frame_index;
    track.video_width = key.video_width;
    track.video_height = key.video_height;
    track.mode = key.mode;
    track.opacity_permille = key.opacity_permille;
    track.show_grid = key.show_grid;
    track.show_qp = key.show_qp;
    track.show_pred = key.show_pred;
    track.show_lines = key.show_lines;
    track.show_bit_cost = key.show_bit_cost;
    track.heatmap_mode = bit_cost_primary
        ? kAnalysisOverlayHeatmapBitCost
        : (qp_primary ? kAnalysisOverlayHeatmapQp : kAnalysisOverlayHeatmapNone);
    const bool qp_feature_available =
        (frame.feature_flags & VACHUNK_FEATURE_QP) == VACHUNK_FEATURE_QP;
    const bool bit_cost_feature_available =
        (frame.feature_flags & VACHUNK_FEATURE_BIT_COST) == VACHUNK_FEATURE_BIT_COST;
    track.missing_qp_feature = qp_primary && !qp_feature_available;
    track.missing_bit_cost_feature = bit_cost_primary && !bit_cost_feature_available;
    track.line_alpha = line_alpha;
    track.fill_rects.reserve(frame.cus.size());
    track.outline_rects.reserve(frame.cus.size());
    track.motion_lines.reserve(key.show_lines ? frame.cus.size() : 0);

    for (const auto& cu : frame.cus) {
        const auto& c = cu.common;
        const int x0 = std::clamp(static_cast<int>(c.x), 0, key.video_width);
        const int y0 = std::clamp(static_cast<int>(c.y), 0, key.video_height);
        const int x1 = std::clamp(static_cast<int>(c.x + c.w), 0, key.video_width);
        const int y1 = std::clamp(static_cast<int>(c.y + c.h), 0, key.video_height);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }

        if (bit_cost_primary && bit_cost_feature_available && fill_alpha > 0) {
            const uint64_t density = analysis::cu_bit_density_normalized_64x64(c);
            if (density > analysis::kOverlayBitDensityHeatmapMax) {
                ++track.heatmap_clamped_bit_cost_count;
            }
            track.fill_rects.push_back(
                {x0, y0, x1, y1, analysis::cu_bit_density_color(c, fill_alpha)});
            ++track.heatmap_rect_count;
        } else if (qp_primary && qp_feature_available && fill_alpha > 0) {
            if (c.qp != analysis::qp_heatmap_clamped_value(c.qp)) {
                ++track.heatmap_clamped_qp_count;
            }
            track.fill_rects.push_back({x0, y0, x1, y1, analysis::qp_color(c.qp, fill_alpha)});
            ++track.heatmap_rect_count;
        } else if (pred_primary && fill_alpha > 0) {
            track.fill_rects.push_back(
                {x0,
                 y0,
                 x1,
                 y1,
                 c.pred_mode == 1
                     ? analysis::OverlayColor{80, 235, 90,
                                               static_cast<uint8_t>(fill_alpha * 3 / 4)}
                     : analysis::pred_color(
                           c.pred_mode, cu.inter,
                           static_cast<uint8_t>(fill_alpha * 3 / 4))});
        }

        if (needs_outlines) {
            track.outline_rects.push_back(
                {x0, y0, x1, y1, analysis::OverlayColor{255, 255, 255, line_alpha}});
        }

        if (key.show_lines && line_alpha > 0 && c.pred_mode != 1) {
            const int cx = (x0 + x1) / 2;
            const int cy = (y0 + y1) / 2;
            const int dx = std::clamp(static_cast<int>(cu.inter.mv_l0_x / 16), -80, 80);
            const int dy = std::clamp(static_cast<int>(cu.inter.mv_l0_y / 16), -80, 80);
            track.motion_lines.push_back(
                {cx,
                 cy,
                 std::clamp(cx + dx, 0, key.video_width),
                 std::clamp(cy + dy, 0, key.video_height),
                 analysis::OverlayColor{80, 180, 255, line_alpha}});
        }
    }

    return track;
}

} // namespace

std::shared_ptr<const AnalysisOverlayPrimitivePackage>
build_analysis_overlay_primitive_package(const RendererDrawSnapshot& snapshot) {

    auto& manager = analysis::AnalysisManager::instance();
    const auto& overlay = manager.overlay_state();
    const bool show_grid = overlay.show_cu_grid.load(std::memory_order_acquire);
    const bool show_qp = overlay.show_qp_heatmap.load(std::memory_order_acquire);
    const bool show_pred = overlay.show_pred_mode.load(std::memory_order_acquire);
    const bool show_lines = overlay.show_pred_lines.load(std::memory_order_acquire);
    const bool show_bit_cost =
        overlay.show_cu_bit_cost_heatmap.load(std::memory_order_acquire);
    const int mode = overlay.mode.load(std::memory_order_acquire);
    const int file_id = overlay.track_file_id.load(std::memory_order_acquire);
    const int opacity_permille =
        std::clamp(overlay.opacity_permille.load(std::memory_order_acquire), 0, 1000);

    if (!show_grid && !show_qp && !show_pred && !show_lines && !show_bit_cost &&
        mode != 0 && mode != 1 && mode != 2 && mode != 3 && mode != 4) {
        return empty_overlay_primitive_package();
    }

    auto overlay_tracks = manager.overlay_track_snapshot();
    if (overlay_tracks.empty() && manager.is_loaded() && file_id >= 0) {
        overlay_tracks.emplace_back(file_id, manager.session_snapshot());
    }
    if (overlay_tracks.empty()) {
        return empty_overlay_primitive_package();
    }

    const bool qp_primary = show_qp || mode == 1 || mode == 3;
    const bool bit_cost_primary = show_bit_cost || mode == 2 || mode == 4;
    const bool pred_primary = show_pred;
    const bool heatmap_primary = qp_primary || bit_cost_primary;
    const uint8_t fill_alpha = overlay_fill_alpha(opacity_permille, heatmap_primary);
    const uint8_t line_alpha =
        static_cast<uint8_t>(std::clamp(opacity_permille * 255 / 1000, 0, 255));
    const bool needs_outlines = line_alpha > 0 && (show_grid || mode == 0 || pred_primary);

    OverlayPrimitivePackageCacheKey cache_key;
    std::vector<OverlayPrimitiveBuildSource> sources;
    sources.reserve(overlay_tracks.size());
    for (const auto& [track_file_id, track_analysis] : overlay_tracks) {
        if (!track_analysis) {
            continue;
        }
        const int slot = find_track_slot(snapshot, track_file_id);
        if (slot < 0 || slot >= static_cast<int>(snapshot.tracks.size())) {
            continue;
        }
        if (!snapshot.decision.frames[slot].has_value()) {
            continue;
        }
        const auto& presented_frame = *snapshot.decision.frames[slot];
        int frame_idx = track_analysis->frame_idx_for_source_packet(
            presented_frame.source_packet_pos,
            presented_frame.source_packet_size,
            presented_frame.source_packet_pos >= 0
                ? presented_frame.source_packet_index
                : -1,
            presented_frame.source_packet_pts,
            presented_frame.source_packet_dts);
        if (frame_idx < 0 &&
            presented_frame.frame_identity_mode == FrameIdentityMode::ExactAnalysisFrame) {
            frame_idx = presented_frame.analysis_frame_index;
        }
        if (frame_idx < 0 || frame_idx >= track_analysis->frame_count()) {
            continue;
        }
        const int video_w = static_cast<int>(track_analysis->video_width());
        const int video_h = static_cast<int>(track_analysis->video_height());
        if (video_w <= 0 || video_h <= 0) {
            continue;
        }

        OverlayPrimitiveTrackCacheKey track_key;
        track_key.session = track_analysis.get();
        track_key.track_file_id = track_file_id;
        track_key.slot = slot;
        track_key.frame_index = frame_idx;
        track_key.video_width = video_w;
        track_key.video_height = video_h;
        track_key.mode = mode;
        track_key.opacity_permille = opacity_permille;
        track_key.show_grid = show_grid;
        track_key.show_qp = show_qp;
        track_key.show_pred = show_pred;
        track_key.show_lines = show_lines;
        track_key.show_bit_cost = show_bit_cost;
        cache_key.tracks.push_back(track_key);
        sources.push_back(OverlayPrimitiveBuildSource{track_key, track_analysis});
    }

    if (sources.empty()) {
        return empty_overlay_primitive_package();
    }

    if (const auto cached = lookup_overlay_primitive_package(cache_key)) {
        return cached;
    }

    auto package = std::make_shared<AnalysisOverlayPrimitivePackage>();
    package->cache_generation = next_overlay_primitive_package_generation();
    package->heatmap_mode = bit_cost_primary
        ? kAnalysisOverlayHeatmapBitCost
        : (qp_primary ? kAnalysisOverlayHeatmapQp : kAnalysisOverlayHeatmapNone);
    package->tracks.reserve(sources.size());
    bool has_uncacheable_missing_data = false;
    for (const auto& source : sources) {
        const auto frame = source.analysis->read_overlay_frame(source.key.frame_index);
        if (frame.cus.empty()) {
            ++package->overlay_frame_missing_count;
            has_uncacheable_missing_data = true;
            continue;
        }

        auto track = build_track_primitives(source.key,
                                            frame,
                                            qp_primary,
                                            bit_cost_primary,
                                            pred_primary,
                                            needs_outlines,
                                            fill_alpha,
                                            line_alpha);

        package->heatmap_rect_count += track.heatmap_rect_count;
        package->heatmap_clamped_qp_count += track.heatmap_clamped_qp_count;
        package->heatmap_clamped_bit_cost_count += track.heatmap_clamped_bit_cost_count;
        if (track.missing_qp_feature || track.missing_bit_cost_feature) {
            ++package->heatmap_missing_feature_track_count;
            has_uncacheable_missing_data = true;
        }

        if (!track.fill_rects.empty() ||
            !track.outline_rects.empty() ||
            !track.motion_lines.empty()) {
            package->tracks.push_back(std::move(track));
        }
    }

    if (!has_uncacheable_missing_data && !package->tracks.empty()) {
        store_overlay_primitive_package(std::move(cache_key), package);
    }
    return package;
}

AnalysisOverlayPrimitivePackage build_analysis_overlay_primitives(
    const RendererDrawSnapshot& snapshot) {
    const auto package = build_analysis_overlay_primitive_package(snapshot);
    return package ? *package : AnalysisOverlayPrimitivePackage{};
}

} // namespace vr
