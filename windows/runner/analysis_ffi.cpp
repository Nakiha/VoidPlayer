#include "analysis_ffi.h"
#include "analysis/analysis_manager.h"
#include "analysis/cache/overlay_chunk.h"
#include "analysis/cache/vacache_store.h"
#include "analysis/generators/bitstream_indexer.h"
#include "analysis/generators/analysis_generator.h"
#include "analysis/parsers/vac2_parser.h"
#include "common/win_utf8.h"
#include "media/private_cdn_flv_demuxer.h"
#include "utils.h"

#include <spdlog/spdlog.h>
#include <windows.h>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <atomic>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

// Callback registered by video_renderer_plugin to provide current PTS.
// Avoids analysis_ffi needing to know about vr::Renderer.
struct PtsCallbackRegistration {
    const void* owner = nullptr;
    NakiAnalysisPtsCallback callback = nullptr;
};

std::mutex g_pts_callback_mutex;
PtsCallbackRegistration g_pts_callback;

struct AnalysisFfiError {
    int32_t status = NAKI_ANALYSIS_OK;
    std::string message;
};

thread_local AnalysisFfiError g_last_analysis_error;

void set_analysis_error(int32_t status, std::string message) {
    g_last_analysis_error.status = status;
    g_last_analysis_error.message = std::move(message);
}

void set_analysis_ok() {
    set_analysis_error(NAKI_ANALYSIS_OK, "");
}

void naki_analysis_register_pts_callback(NakiAnalysisPtsCallback cb) {
    naki_analysis_register_pts_callback_for_owner(nullptr, cb);
}

void naki_analysis_register_pts_callback_for_owner(
    const void* owner,
    NakiAnalysisPtsCallback cb) {
    std::lock_guard<std::mutex> lock(g_pts_callback_mutex);
    g_pts_callback = PtsCallbackRegistration{owner, cb};
}

void naki_analysis_clear_pts_callback_for_owner(const void* owner) {
    std::lock_guard<std::mutex> lock(g_pts_callback_mutex);
    if (g_pts_callback.owner == owner) {
        g_pts_callback = PtsCallbackRegistration{};
    }
}

int32_t naki_analysis_abi_version() {
    return NAKI_ANALYSIS_ABI_VERSION;
}

int32_t naki_analysis_last_error(char* buf, int32_t cap) {
    if (buf && cap > 0) {
        const auto& message = g_last_analysis_error.message;
        const size_t writable = static_cast<size_t>(cap - 1);
        const size_t to_copy = std::min(writable, message.size());
        std::memcpy(buf, message.data(), to_copy);
        buf[to_copy] = '\0';
    }
    return g_last_analysis_error.status;
}

int32_t naki_analysis_sizeof_summary() {
    return static_cast<int32_t>(sizeof(NakiAnalysisSummary));
}

int32_t naki_analysis_sizeof_frame_info() {
    return static_cast<int32_t>(sizeof(NakiFrameInfo));
}

int32_t naki_analysis_sizeof_nalu_info() {
    return static_cast<int32_t>(sizeof(NakiNaluInfo));
}

int32_t naki_analysis_sizeof_frame_bucket() {
    return static_cast<int32_t>(sizeof(NakiFrameBucket));
}

int32_t naki_analysis_sizeof_overlay_state() {
    return static_cast<int32_t>(sizeof(NakiOverlayState));
}

int32_t naki_analysis_sizeof_summary_v2() {
    return static_cast<int32_t>(sizeof(NakiAnalysisSummaryV2));
}

int32_t naki_analysis_sizeof_frame_info_v2() {
    return static_cast<int32_t>(sizeof(NakiFrameInfoV2));
}

int32_t naki_analysis_sizeof_nalu_info_v2() {
    return static_cast<int32_t>(sizeof(NakiNaluInfoV2));
}

int32_t naki_analysis_sizeof_frame_bucket_v2() {
    return static_cast<int32_t>(sizeof(NakiFrameBucketV2));
}

int32_t naki_analysis_sizeof_overlay_state_v2() {
    return static_cast<int32_t>(sizeof(NakiOverlayStateV2));
}

namespace {

std::mutex g_analysis_mutex;
std::mutex g_analysis_generate_mutex;

const char* safe_cstr(const char* value) {
    return value ? value : "";
}

struct AnalysisHandleState {
    std::unique_ptr<vr::analysis::Vac2BaseFile> vac2_base;
    std::vector<Vac2FrameSummaryEntry> exact_frame_summaries;
    std::vector<uint8_t> exact_frame_summary_present;
    std::mutex mutex;
    bool closed = false;
};

std::mutex g_handle_registry_mutex;
std::unordered_map<uintptr_t, std::shared_ptr<AnalysisHandleState>> g_handle_registry;
std::atomic<uintptr_t> g_next_handle_id{1};

struct FfmpegOpenTimeout {
    int64_t deadline_ns = 0;
};

int64_t steady_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int ffmpeg_interrupt_callback(void* opaque) {
    auto* timeout = static_cast<FfmpegOpenTimeout*>(opaque);
    if (!timeout || timeout->deadline_ns <= 0) {
        return 0;
    }
    return steady_clock_ns() > timeout->deadline_ns ? 1 : 0;
}

AVFormatContext* alloc_format_context_with_timeout(FfmpegOpenTimeout& timeout,
                                                   std::chrono::seconds duration) {
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        return nullptr;
    }
    timeout.deadline_ns = steady_clock_ns() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    fmt_ctx->interrupt_callback.callback = &ffmpeg_interrupt_callback;
    fmt_ctx->interrupt_callback.opaque = &timeout;
    return fmt_ctx;
}

NakiAnalysisHandle encode_analysis_handle(uintptr_t id) {
    return reinterpret_cast<NakiAnalysisHandle>(id);
}

uintptr_t decode_analysis_handle(NakiAnalysisHandle handle) {
    return reinterpret_cast<uintptr_t>(handle);
}

std::shared_ptr<AnalysisHandleState> pin_analysis_handle(NakiAnalysisHandle handle) {
    const uintptr_t id = decode_analysis_handle(handle);
    if (id == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_handle_registry_mutex);
    auto it = g_handle_registry.find(id);
    return it != g_handle_registry.end() ? it->second : nullptr;
}

NakiAnalysisHandle register_analysis_handle(std::shared_ptr<AnalysisHandleState> state) {
    if (!state) return nullptr;
    try {
        uintptr_t id = g_next_handle_id.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) {
            id = g_next_handle_id.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lock(g_handle_registry_mutex);
            while (id == 0 || g_handle_registry.find(id) != g_handle_registry.end()) {
                id = g_next_handle_id.fetch_add(1, std::memory_order_relaxed);
            }
            g_handle_registry.emplace(id, std::move(state));
        }
        return encode_analysis_handle(id);
    } catch (const std::exception& e) {
        spdlog::error("[analysis_ffi] failed to register analysis handle: {}", e.what());
    } catch (...) {
        spdlog::error("[analysis_ffi] failed to register analysis handle: unknown exception");
    }
    return nullptr;
}

std::shared_ptr<AnalysisHandleState> unregister_analysis_handle(NakiAnalysisHandle handle) {
    const uintptr_t id = decode_analysis_handle(handle);
    if (id == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_handle_registry_mutex);
    auto it = g_handle_registry.find(id);
    if (it == g_handle_registry.end()) return nullptr;
    auto state = std::move(it->second);
    g_handle_registry.erase(it);
    return state;
}

bool file_starts_with_magic_utf8(const char* path, const char (&magic)[4]) {
    if (!path || path[0] == '\0') return false;
    std::ifstream in(vr::win_utf8::path_from_utf8(path), std::ios::binary);
    if (!in.is_open()) return false;
    char got[4] = {};
    in.read(got, sizeof(got));
    return in.gcount() == static_cast<std::streamsize>(sizeof(got)) &&
           std::memcmp(got, magic, sizeof(got)) == 0;
}

bool is_vac2_base_path(const char* path) {
    static constexpr char kMagic[4] = {'V', 'A', 'C', '2'};
    return file_starts_with_magic_utf8(path, kMagic);
}

bool read_record_section_bytes(const vr::analysis::VachunkFile& chunk,
                               const char (&type)[5],
                               uint32_t entry_size,
                               std::vector<uint8_t>& out) {
    out.clear();
    const auto* section = chunk.section(type);
    if (!section ||
        section->entry_size != entry_size ||
        section->decoded_size != static_cast<uint64_t>(section->entry_count) * entry_size ||
        section->decoded_size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    return chunk.read_section(type, out) && out.size() == section->decoded_size;
}

void load_exact_frame_summary_chunks(AnalysisHandleState& state,
                                     const char* analysis_path) {
    state.exact_frame_summaries.clear();
    state.exact_frame_summary_present.clear();
    if (!state.vac2_base || !analysis_path || analysis_path[0] == '\0') return;

    const auto frame_count = state.vac2_base->frames().size();
    if (frame_count == 0) return;
    state.exact_frame_summaries.resize(frame_count);
    state.exact_frame_summary_present.assign(frame_count, 0);

    const auto base_path = vr::win_utf8::path_from_utf8(analysis_path);
    const auto exact_dir = base_path.parent_path() / L"chunks" / L"frame_summary_exact";
    std::error_code ec;
    if (!std::filesystem::exists(exact_dir, ec) || ec) return;

    const auto& base_header = state.vac2_base->header();
    std::vector<uint64_t> generator_by_frame(frame_count, 0);
    for (const auto& entry : std::filesystem::directory_iterator(exact_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec || entry.path().extension() != L".vck") {
            ec.clear();
            continue;
        }

        vr::analysis::VachunkFile chunk;
        if (!chunk.open(vr::win_utf8::path_to_utf8(entry.path()))) continue;
        const auto& header = chunk.header();
        if (header.kind != static_cast<uint16_t>(VachunkKind::FrameSummaryExact) ||
            header.codec != base_header.codec ||
            header.track_index != base_header.track_index ||
            header.base_content_revision != base_header.content_revision ||
            header.start_frame > header.end_frame ||
            header.end_frame >= frame_count) {
            continue;
        }

        std::vector<uint8_t> bytes;
        if (!read_record_section_bytes(chunk, "FSUM", sizeof(Vac2FrameSummaryEntry), bytes)) {
            continue;
        }
        const auto count = bytes.size() / sizeof(Vac2FrameSummaryEntry);
        const auto expected_count =
            static_cast<size_t>(header.end_frame - header.start_frame + 1);
        if (count != expected_count) continue;

        const auto* summaries =
            reinterpret_cast<const Vac2FrameSummaryEntry*>(bytes.data());
        for (size_t i = 0; i < count; ++i) {
            const size_t frame = static_cast<size_t>(header.start_frame) + i;
            if (!state.exact_frame_summary_present[frame] ||
                header.generator_revision >= generator_by_frame[frame]) {
                state.exact_frame_summaries[frame] = summaries[i];
                state.exact_frame_summary_present[frame] = 1;
                generator_by_frame[frame] = header.generator_revision;
            }
        }
    }
}

bool open_analysis_handle_path(AnalysisHandleState& state, const char* analysis_path) {
    if (!is_vac2_base_path(analysis_path)) return false;
    auto base = std::make_unique<vr::analysis::Vac2BaseFile>();
    if (!base->open(analysis_path)) return false;
    state.vac2_base = std::move(base);
    load_exact_frame_summary_chunks(state, analysis_path);
    return true;
}

int32_t clamp_count_to_i32(size_t count) {
    return count > static_cast<size_t>(std::numeric_limits<int32_t>::max())
        ? std::numeric_limits<int32_t>::max()
        : static_cast<int32_t>(count);
}

bool fill_vac2_frame_at(const vr::analysis::Vac2BaseFile& base,
                        const std::vector<Vac2FrameSummaryEntry>* exact_summaries,
                        const std::vector<uint8_t>* exact_present,
                        int32_t source_index,
                        NakiFrameInfo& f);
int32_t fill_vac2_nalus_range(const vr::analysis::Vac2BaseFile& base,
                              int32_t start,
                              NakiNaluInfo* out,
                              int32_t max_count);
int32_t vac2_frame_to_nalu(const vr::analysis::Vac2BaseFile& base,
                           int32_t frame_index);
int32_t vac2_nalu_to_frame(const vr::analysis::Vac2BaseFile& base,
                           int32_t nalu_index);
int32_t fill_vac2_frame_buckets(const vr::analysis::Vac2BaseFile& base,
                                int32_t start,
                                int32_t bucket_size,
                                NakiFrameBucket* out,
                                int32_t max_count);

int32_t vac2_frame_idx_for_pts_us(const vr::analysis::Vac2BaseFile& base,
                                  int64_t pts_us) {
    const auto& frames = base.frames();
    if (frames.empty()) return -1;

    const auto& h = base.header();
    if (h.time_base_num <= 0 || h.time_base_den <= 0) return -1;
    const long double target_units =
        (static_cast<long double>(pts_us) * h.time_base_den) /
        (static_cast<long double>(h.time_base_num) * 1000000.0L);
    const int64_t target_pts = static_cast<int64_t>(target_units);

    auto it = std::upper_bound(
        frames.begin(),
        frames.end(),
        target_pts,
        [](int64_t value, const Vac2FrameEntry& frame) {
            return value < frame.pts;
        });
    if (it == frames.begin()) return 0;
    return static_cast<int32_t>(std::distance(frames.begin(), it) - 1);
}

int32_t vac2_frame_idx_for_timestamp_us(const vr::analysis::Vac2BaseFile& base,
                                        int64_t pts_us,
                                        int64_t dts_us) {
    const auto& frames = base.frames();
    if (frames.empty()) return -1;

    const auto& h = base.header();
    if (h.time_base_num <= 0 || h.time_base_den <= 0) return -1;
    const AVRational stream_time_base{h.time_base_num, h.time_base_den};
    const AVRational us_time_base{1, 1000000};

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        const int64_t frame_pts_us =
            av_rescale_q(frame.pts, stream_time_base, us_time_base);
        const int64_t frame_dts_us =
            av_rescale_q(frame.dts, stream_time_base, us_time_base);
        if (frame_pts_us == pts_us && frame_dts_us == dts_us) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

int32_t current_vac2_frame_idx(const vr::analysis::Vac2BaseFile& base) {
    NakiAnalysisPtsCallback cb = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_pts_callback_mutex);
        cb = g_pts_callback.callback;
    }
    if (!cb) return -1;
    return vac2_frame_idx_for_pts_us(base, cb());
}

void fill_vac2_summary(const vr::analysis::Vac2BaseFile& base,
                       NakiAnalysisSummary& s) {
    std::memset(&s, 0, sizeof(s));
    s.loaded = 1;
    s.current_frame_idx = current_vac2_frame_idx(base);
    const auto& h = base.header();
    s.frame_count = clamp_count_to_i32(base.frames().size());
    s.packet_count = clamp_count_to_i32(base.packets().size());
    s.nalu_count = clamp_count_to_i32(base.units().size());
    s.video_width = static_cast<int32_t>(std::min<uint32_t>(
        h.width, static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
    s.video_height = static_cast<int32_t>(std::min<uint32_t>(
        h.height, static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
    s.time_base_num = h.time_base_num;
    s.time_base_den = h.time_base_den;
    s.codec = static_cast<int32_t>(h.codec);
}

bool fill_vac2_frame_at(const vr::analysis::Vac2BaseFile& base,
                        const std::vector<Vac2FrameSummaryEntry>* exact_summaries,
                        const std::vector<uint8_t>* exact_present,
                        int32_t source_index,
                        NakiFrameInfo& f) {
    if (source_index < 0 ||
        static_cast<size_t>(source_index) >= base.frames().size()) {
        return false;
    }

    const auto& frame = base.frames()[source_index];
    const Vac2FrameSummaryEntry* summary = nullptr;
    const auto exact_index = static_cast<size_t>(source_index);
    if (exact_summaries &&
        exact_present &&
        exact_index < exact_summaries->size() &&
        exact_index < exact_present->size() &&
        (*exact_present)[exact_index]) {
        summary = &(*exact_summaries)[exact_index];
    } else if (static_cast<size_t>(source_index) < base.frame_summaries().size()) {
        summary = &base.frame_summaries()[source_index];
    }

    std::memset(&f, 0, sizeof(f));
    f.poc = summary ? summary->poc : frame.poc;
    f.temporal_id = summary ? summary->temporal_id : 0;
    f.slice_type = summary ? summary->slice_type : 255;
    f.nal_type = summary ? summary->nal_type : 0;
    if (summary && summary->qp_kind != VAC2_QP_KIND_UNKNOWN) {
        f.avg_qp = summary->qp_avg;
    }
    if (summary) {
        f.num_ref_l0 = std::min<int32_t>(summary->num_ref_l0, 15);
        f.num_ref_l1 = std::min<int32_t>(summary->num_ref_l1, 15);
        std::memcpy(f.ref_pocs_l0, summary->ref_pocs_l0, sizeof(f.ref_pocs_l0));
        std::memcpy(f.ref_pocs_l1, summary->ref_pocs_l1, sizeof(f.ref_pocs_l1));
    }
    f.pts = frame.pts;
    f.dts = frame.dts;
    f.packet_size = static_cast<int32_t>(std::min<uint32_t>(
        frame.frame_size, static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
    f.keyframe = (frame.flags & VAC2_FRAME_FLAG_KEYFRAME) ? 1 : 0;
    return true;
}

int32_t fill_vac2_frames_range(const vr::analysis::Vac2BaseFile& base,
                               const std::vector<Vac2FrameSummaryEntry>* exact_summaries,
                               const std::vector<uint8_t>* exact_present,
                               int32_t start,
                               NakiFrameInfo* out,
                               int32_t max_count) {
    if (!out || max_count <= 0 || start < 0) return 0;
    const int32_t total_count = clamp_count_to_i32(base.frames().size());
    if (start >= total_count) return 0;
    const int32_t count = std::min(max_count, total_count - start);
    for (int32_t i = 0; i < count; ++i) {
        if (!fill_vac2_frame_at(
                base, exact_summaries, exact_present, start + i, out[i])) {
            return i;
        }
    }
    return count;
}

int32_t fill_vac2_nalus_range(const vr::analysis::Vac2BaseFile& base,
                              int32_t start,
                              NakiNaluInfo* out,
                              int32_t max_count) {
    if (!out || max_count <= 0 || start < 0) return 0;
    const int32_t total_count = clamp_count_to_i32(base.units().size());
    if (start >= total_count) return 0;
    const int32_t count = std::min(max_count, total_count - start);
    for (int32_t i = 0; i < count; ++i) {
        const auto& unit = base.units()[start + i];
        auto& n = out[i];
        n.offset = unit.offset;
        n.size = unit.size;
        n.nal_type = unit.nal_type;
        n.temporal_id = unit.temporal_id;
        n.layer_id = unit.layer_id;
        n.flags = 0;
        if (unit.flags & VAC2_UNIT_FLAG_IS_VCL) n.flags |= 0x01;
        if (unit.flags & VAC2_UNIT_FLAG_IS_SLICE) n.flags |= 0x02;
        if (unit.flags & VAC2_UNIT_FLAG_IS_KEYFRAME) n.flags |= 0x04;
    }
    return count;
}

int32_t vac2_frame_to_nalu(const vr::analysis::Vac2BaseFile& base,
                           int32_t frame_index) {
    if (frame_index < 0 ||
        static_cast<size_t>(frame_index) >= base.frames().size()) {
        return -1;
    }
    if (static_cast<size_t>(frame_index) < base.frame_summaries().size()) {
        const auto first_vcl = base.frame_summaries()[frame_index].first_vcl_unit;
        if (first_vcl != UINT32_MAX && first_vcl < base.units().size()) {
            return static_cast<int32_t>(first_vcl);
        }
    }
    const auto first_unit = base.frames()[frame_index].first_unit;
    if (first_unit == UINT32_MAX || first_unit >= base.units().size()) return -1;
    return static_cast<int32_t>(first_unit);
}

int32_t vac2_nalu_to_frame(const vr::analysis::Vac2BaseFile& base,
                           int32_t nalu_index) {
    if (nalu_index < 0 ||
        static_cast<size_t>(nalu_index) >= base.units().size()) {
        return -1;
    }
    const auto& unit = base.units()[nalu_index];
    if (unit.au_index != UINT32_MAX && unit.au_index < base.frames().size()) {
        return static_cast<int32_t>(unit.au_index);
    }
    const uint32_t needle = static_cast<uint32_t>(nalu_index);
    const auto& frames = base.frames();
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        const uint64_t start = frame.first_unit;
        const uint64_t end = start + frame.unit_count;
        if (start != UINT32_MAX && needle >= start && needle < end) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

int32_t fill_vac2_frame_buckets(const vr::analysis::Vac2BaseFile& base,
                                int32_t start,
                                int32_t bucket_size,
                                NakiFrameBucket* out,
                                int32_t max_count) {
    if (!out || max_count <= 0 || bucket_size <= 0 || start < 0) return 0;
    const int32_t total_count = clamp_count_to_i32(base.frames().size());
    if (start >= total_count) return 0;

    int32_t produced = 0;
    int32_t bucket_start = start;
    while (produced < max_count && bucket_start < total_count) {
        const int32_t count = std::min(bucket_size, total_count - bucket_start);
        auto& bucket = out[produced];
        std::memset(&bucket, 0, sizeof(bucket));
        bucket.start_frame = bucket_start;
        bucket.frame_count = count;
        bucket.packet_size_min = std::numeric_limits<int32_t>::max();
        bucket.qp_min = std::numeric_limits<int32_t>::max();

        for (int32_t i = 0; i < count; ++i) {
            NakiFrameInfo f{};
            if (!fill_vac2_frame_at(base, nullptr, nullptr, bucket_start + i, f)) break;
            bucket.packet_size_min = std::min(bucket.packet_size_min, f.packet_size);
            bucket.packet_size_max = std::max(bucket.packet_size_max, f.packet_size);
            bucket.packet_size_sum += f.packet_size;
            bucket.qp_min = std::min(bucket.qp_min, f.avg_qp);
            bucket.qp_max = std::max(bucket.qp_max, f.avg_qp);
            bucket.qp_sum += f.avg_qp;
            if (f.keyframe != 0) bucket.keyframe_count++;
        }
        if (bucket.packet_size_min == std::numeric_limits<int32_t>::max()) {
            bucket.packet_size_min = 0;
        }
        if (bucket.qp_min == std::numeric_limits<int32_t>::max()) {
            bucket.qp_min = 0;
        }

        ++produced;
        bucket_start += count;
    }
    return produced;
}

} // namespace

extern "C" __declspec(dllexport)
void naki_analysis_set_overlay(const NakiOverlayState* state) {
    if (!state) return;
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& overlay = vr::analysis::AnalysisManager::instance().overlay;
    overlay.show_cu_grid.store(state->show_cu_grid != 0, std::memory_order_release);
    overlay.show_pred_mode.store(state->show_pred_mode != 0, std::memory_order_release);
    overlay.show_qp_heatmap.store(state->show_qp_heatmap != 0, std::memory_order_release);
    overlay.show_pred_lines.store(state->show_pred_lines != 0, std::memory_order_release);
    overlay.show_cu_bit_cost_heatmap.store(state->show_cu_bit_cost_heatmap != 0, std::memory_order_release);
    overlay.opacity_permille.store(std::clamp(state->opacity_permille, 0, 1000), std::memory_order_release);
    overlay.mode.store(std::max(0, state->mode), std::memory_order_release);
    overlay.track_file_id.store(state->track_file_id, std::memory_order_release);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_set_overlay_track(int32_t track_file_id, const char* analysis_path) {
    if (track_file_id < 0 || !analysis_path) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "track_file_id and analysis_path are required");
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& mgr = vr::analysis::AnalysisManager::instance();
    if (!mgr.set_overlay_track(track_file_id, safe_cstr(analysis_path))) {
        set_analysis_error(NAKI_ANALYSIS_ERR_OPEN_FAILED, "failed to set overlay track");
        return 0;
    }
    set_analysis_ok();
    return 1;
}

extern "C" __declspec(dllexport)
void naki_analysis_clear_overlay_tracks() {
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    vr::analysis::AnalysisManager::instance().clear_overlay_tracks();
}

extern "C" __declspec(dllexport)
NakiAnalysisHandle naki_analysis_open(const char* analysis_path) {
    try {
        if (!analysis_path || analysis_path[0] == '\0') {
            set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "analysis_path is required");
            return nullptr;
        }
        auto state = std::shared_ptr<AnalysisHandleState>(new (std::nothrow) AnalysisHandleState());
        if (!state) {
            set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL, "failed to allocate analysis handle");
            return nullptr;
        }
        if (!open_analysis_handle_path(*state, analysis_path)) {
            set_analysis_error(NAKI_ANALYSIS_ERR_OPEN_FAILED, "failed to load analysis container");
            return nullptr;
        }
        auto handle = register_analysis_handle(state);
        if (!handle) {
            if (state->vac2_base) state->vac2_base->close();
            state->vac2_base.reset();
            set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL, "failed to register analysis handle");
            return nullptr;
        }
        set_analysis_ok();
        return handle;
    } catch (const std::exception& e) {
        spdlog::error("[analysis_ffi] naki_analysis_open failed: {}", e.what());
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           std::string("naki_analysis_open exception: ") + e.what());
    } catch (...) {
        spdlog::error("[analysis_ffi] naki_analysis_open failed: unknown exception");
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "naki_analysis_open unknown exception");
    }
    return nullptr;
}

extern "C" __declspec(dllexport)
void naki_analysis_close(NakiAnalysisHandle handle) {
    auto state = unregister_analysis_handle(handle);
    if (!state) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    if (state->vac2_base) state->vac2_base->close();
    state->vac2_base.reset();
    state->exact_frame_summaries.clear();
    state->exact_frame_summary_present.clear();
    set_analysis_ok();
}

extern "C" __declspec(dllexport)
const NakiAnalysisSummary* naki_analysis_handle_get_summary(NakiAnalysisHandle handle) {
    thread_local NakiAnalysisSummary summary{};
    auto state = pin_analysis_handle(handle);
    if (!state) {
        std::memset(&summary, 0, sizeof(summary));
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return &summary;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        std::memset(&summary, 0, sizeof(summary));
        set_analysis_error(NAKI_ANALYSIS_ERR_CLOSED, "analysis handle is closed");
    } else {
        fill_vac2_summary(*state->vac2_base, summary);
        set_analysis_ok();
    }
    return &summary;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_frame_index_for_timestamp(NakiAnalysisHandle handle,
                                                       int64_t pts_us,
                                                       int64_t dts_us) {
    auto state = pin_analysis_handle(handle);
    if (!state) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return -1;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        set_analysis_error(NAKI_ANALYSIS_ERR_CLOSED, "analysis handle is closed");
        return -1;
    }
    const auto index = vac2_frame_idx_for_timestamp_us(
        *state->vac2_base, pts_us, dts_us);
    set_analysis_ok();
    return index;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frames(NakiAnalysisHandle handle, NakiFrameInfo* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return 0;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        set_analysis_error(NAKI_ANALYSIS_ERR_CLOSED, "analysis handle is closed");
        return 0;
    }
    auto count = fill_vac2_frames_range(
        *state->vac2_base,
        &state->exact_frame_summaries,
        &state->exact_frame_summary_present,
        0,
        out,
        max_count);
    set_analysis_ok();
    return count;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frames_range(NakiAnalysisHandle handle, int32_t start, NakiFrameInfo* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return 0;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        set_analysis_error(NAKI_ANALYSIS_ERR_CLOSED, "analysis handle is closed");
        return 0;
    }
    auto count = fill_vac2_frames_range(
        *state->vac2_base,
        &state->exact_frame_summaries,
        &state->exact_frame_summary_present,
        start,
        out,
        max_count);
    set_analysis_ok();
    return count;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_nalus(NakiAnalysisHandle handle, NakiNaluInfo* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return 0;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        set_analysis_error(NAKI_ANALYSIS_ERR_CLOSED, "analysis handle is closed");
        return 0;
    }
    auto count = fill_vac2_nalus_range(*state->vac2_base, 0, out, max_count);
    set_analysis_ok();
    return count;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_nalus_range(NakiAnalysisHandle handle, int32_t start, NakiNaluInfo* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return 0;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        set_analysis_error(NAKI_ANALYSIS_ERR_CLOSED, "analysis handle is closed");
        return 0;
    }
    auto count = fill_vac2_nalus_range(*state->vac2_base, start, out, max_count);
    set_analysis_ok();
    return count;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_frame_to_nalu(NakiAnalysisHandle handle, int32_t frame_index) {
    auto state = pin_analysis_handle(handle);
    if (!state) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return -1;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        set_analysis_error(NAKI_ANALYSIS_ERR_CLOSED, "analysis handle is closed");
        return -1;
    }
    auto index = vac2_frame_to_nalu(*state->vac2_base, frame_index);
    set_analysis_ok();
    return index;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_nalu_to_frame(NakiAnalysisHandle handle, int32_t nalu_index) {
    auto state = pin_analysis_handle(handle);
    if (!state) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return -1;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        set_analysis_error(NAKI_ANALYSIS_ERR_CLOSED, "analysis handle is closed");
        return -1;
    }
    auto index = vac2_nalu_to_frame(*state->vac2_base, nalu_index);
    set_analysis_ok();
    return index;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frame_buckets(NakiAnalysisHandle handle, int32_t start, int32_t bucket_size, NakiFrameBucket* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "analysis handle is invalid or closed");
        return 0;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        set_analysis_error(NAKI_ANALYSIS_ERR_CLOSED, "analysis handle is closed");
        return 0;
    }
    auto count = fill_vac2_frame_buckets(
        *state->vac2_base, start, bucket_size, out, max_count);
    set_analysis_ok();
    return count;
}

// ---- Analysis generation ----

static std::string get_exe_dir() {
    return vr::win_utf8::module_directory_utf8();
}

static uint64_t file_size_utf8(const std::string& path) {
    if (path.empty()) return 0;
    std::error_code ec;
    const auto size = std::filesystem::file_size(vr::win_utf8::path_from_utf8(path), ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

static bool ends_with_ascii(const std::string& text, const char* suffix) {
    const size_t suffix_len = std::strlen(suffix);
    return text.size() >= suffix_len &&
        text.compare(text.size() - suffix_len, suffix_len, suffix) == 0;
}

static bool is_analysis_cache_artifact_name(const std::string& name) {
    return ends_with_ascii(name, ".vac") ||
        ends_with_ascii(name, ".vck");
}

static bool is_current_hash_artifact_name(const std::string& name, const char* hash) {
    if (!hash || hash[0] == '\0') return false;
    const std::string prefix = std::string(hash) + ".";
    return name.rfind(prefix, 0) == 0 && is_analysis_cache_artifact_name(name);
}

static uint64_t directory_tree_bytes_utf8(const std::string& path);

struct NativeCacheBudget {
    bool limited = false;
    uint64_t available_for_current_hash = 0;
};

static NativeCacheBudget compute_native_cache_budget(const std::string& data_dir,
                                                     const char* hash,
                                                     int64_t max_cache_bytes) {
    NativeCacheBudget budget{};
    if (max_cache_bytes <= 0) return budget;
    budget.limited = true;

    uint64_t other_bytes = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             vr::win_utf8::path_from_utf8(data_dir), ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const std::string name = vr::win_utf8::path_to_utf8(entry.path().filename());
        if (!is_analysis_cache_artifact_name(name) ||
            is_current_hash_artifact_name(name, hash)) {
            continue;
        }
        const auto size = entry.file_size(ec);
        if (!ec) {
            other_bytes += static_cast<uint64_t>(size);
        }
        ec.clear();
    }

    const auto max_bytes = static_cast<uint64_t>(max_cache_bytes);
    budget.available_for_current_hash =
        other_bytes >= max_bytes ? 0 : max_bytes - other_bytes;
    return budget;
}

static uint64_t current_hash_artifact_bytes(const std::string& data_dir,
                                            const char* hash,
                                            const std::string& staging_dir = {}) {
    if (!hash || hash[0] == '\0') return 0;
    uint64_t total = 0;
    const std::vector<const char*> suffixes = {
        ".vac",
    };
    for (const auto* suffix : suffixes) {
        total += file_size_utf8(data_dir + "\\" + hash + suffix);
    }
    if (!staging_dir.empty()) {
        total += directory_tree_bytes_utf8(staging_dir);
    }
    return total;
}

static bool check_current_hash_budget(const std::string& data_dir,
                                      const char* hash,
                                      const NativeCacheBudget& budget,
                                      const char* stage,
                                      const std::string& staging_dir = {}) {
    if (!budget.limited) return true;
    const uint64_t used = current_hash_artifact_bytes(data_dir, hash, staging_dir);
    if (used <= budget.available_for_current_hash) return true;
    spdlog::warn("[Analysis] cache limit exceeded during {}: current_hash={} available={}",
                 stage, used, budget.available_for_current_hash);
    return false;
}

static uint64_t remaining_current_hash_budget(const std::string& data_dir,
                                              const char* hash,
                                              const NativeCacheBudget& budget,
                                              const std::string& staging_dir = {}) {
    if (!budget.limited) return 0;
    const uint64_t used = current_hash_artifact_bytes(data_dir, hash, staging_dir);
    return used >= budget.available_for_current_hash
        ? 0
        : budget.available_for_current_hash - used;
}

static uint64_t overlay_publish_output_budget(const std::string& data_dir,
                                              const char* hash,
                                              const NativeCacheBudget& budget) {
    return budget.limited ? remaining_current_hash_budget(data_dir, hash, budget) : 0;
}

#ifdef VOIDPLAYER_ANALYSIS_FFI_TESTING
extern "C" uint64_t naki_analysis_test_overlay_publish_budget(
    const char* data_dir,
    const char* hash,
    int64_t max_cache_bytes) {
    if (!data_dir || !hash) return 0;
    const NativeCacheBudget budget =
        compute_native_cache_budget(data_dir, hash, max_cache_bytes);
    return overlay_publish_output_budget(data_dir, hash, budget);
}
#endif

static uint64_t watched_file_bytes(const std::vector<std::string>& paths) {
    uint64_t total = 0;
    for (const auto& path : paths) {
        total += file_size_utf8(path);
    }
    return total;
}

static uint64_t directory_tree_bytes_utf8(const std::string& path) {
    uint64_t total = 0;
    std::error_code ec;
    const auto root = vr::win_utf8::path_from_utf8(path);
    if (!std::filesystem::exists(root, ec) || ec) return 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const auto size = entry.file_size(ec);
        if (!ec) {
            total += static_cast<uint64_t>(size);
        }
        ec.clear();
    }
    return total;
}

static bool remove_directory_tree_utf8(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(vr::win_utf8::path_from_utf8(path), ec);
    return !ec;
}

static AnalysisCodec detect_analysis_codec(const char* video_path) {
    if (vr::PrivateCdnFlvDemuxer::probe(video_path)) {
        vr::PrivateCdnFlvDemuxer demuxer;
        if (demuxer.open(video_path) && demuxer.stats().codec_params) {
            AnalysisCodec codec = vr::analysis::BitstreamIndexer::codec_from_ffmpeg_id(
                demuxer.stats().codec_params->codec_id);
            if (codec != AnalysisCodec::Unknown) return codec;
        }
    }

    FfmpegOpenTimeout timeout;
    AVFormatContext* fmt_ctx = alloc_format_context_with_timeout(timeout, std::chrono::seconds(30));
    if (!fmt_ctx) {
        return vr::analysis::BitstreamIndexer::codec_from_path(video_path);
    }
    int ret = avformat_open_input(&fmt_ctx, video_path, nullptr, nullptr);
    if (ret >= 0) {
        ret = avformat_find_stream_info(fmt_ctx, nullptr);
        timeout.deadline_ns = 0;
        if (ret >= 0) {
            for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
                const auto* codecpar = fmt_ctx->streams[i]->codecpar;
                if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    AnalysisCodec codec = vr::analysis::BitstreamIndexer::codec_from_ffmpeg_id(
                        codecpar->codec_id);
                    avformat_close_input(&fmt_ctx);
                    if (codec != AnalysisCodec::Unknown) return codec;
                    return vr::analysis::BitstreamIndexer::codec_from_path(video_path);
                }
            }
        }
        avformat_close_input(&fmt_ctx);
    } else {
        timeout.deadline_ns = 0;
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
    }
    return vr::analysis::BitstreamIndexer::codec_from_path(video_path);
}

// Run a command via CreateProcess. Returns exit code, or -1 on CreateProcess failure.
// If log_path is non-empty, stdout+stderr are redirected to that file.
static int run_command(const std::string& cmd,
                       const std::string& log_path = {},
                       const std::vector<std::string>& watched_paths = {},
                       uint64_t max_watched_bytes = 0) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };  // inheritable
    HANDLE hLogFile = INVALID_HANDLE_VALUE;

    if (!log_path.empty()) {
        const auto wide_log_path = vr::win_utf8::utf16_from_utf8(log_path);
        hLogFile = CreateFileW(wide_log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLogFile != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.hStdOutput = hLogFile;
            si.hStdError = hLogFile;
            si.wShowWindow = SW_HIDE;
        }
    } else {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }

    std::wstring cmdline = vr::win_utf8::utf16_from_utf8(cmd);
    if (cmdline.empty()) {
        if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);
        return -1;
    }
    if (!CreateProcessW(
            nullptr, cmdline.data(),
            nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi)) {
        if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);
        return -1;
    }

    bool killed_for_limit = false;
    while (true) {
        const DWORD wait = WaitForSingleObject(
            pi.hProcess,
            max_watched_bytes > 0 ? 200 : INFINITE);
        if (wait != WAIT_TIMEOUT) break;
        if (watched_file_bytes(watched_paths) > max_watched_bytes) {
            spdlog::warn("[Analysis] command output exceeded cache limit; terminating process");
            TerminateProcess(pi.hProcess, 1);
            killed_for_limit = true;
            break;
        }
    }

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);
    return killed_for_limit ? -2 : static_cast<int>(exit_code);
}

static std::string make_analysis_tool_log_path(const std::string& logs_dir,
                                               const char* tool_tag,
                                               const char* hash) {
    vr::win_utf8::create_directory_utf8(logs_dir);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char log_name[160];
    snprintf(log_name, sizeof(log_name),
             "%s_%04d-%02d-%02d_%02d%02d%02d_%lu_%s.log",
             tool_tag,
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             static_cast<unsigned long>(GetCurrentProcessId()),
             hash);
    return logs_dir + "\\" + log_name;
}

static std::string first_existing_tool_path(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        if (vr::win_utf8::file_exists_utf8(path)) {
            return path;
        }
    }
    return {};
}

class ScopedStagingDir {
public:
    explicit ScopedStagingDir(std::string path) : path_(std::move(path)) {}
    ~ScopedStagingDir() {
        if (active_ && !path_.empty()) {
            remove_directory_tree_utf8(path_);
        }
    }

    const std::string& path() const { return path_; }
    void dismiss() { active_ = false; }

private:
    std::string path_;
    bool active_ = true;
};

static const char* ffmpeg_analysis_codec_arg(AnalysisCodec codec) {
    switch (codec) {
    case AnalysisCodec::VVC:   return "vvc";
    case AnalysisCodec::HEVC:  return "hevc";
    case AnalysisCodec::H264:  return "h264";
    default:              return nullptr;
    }
}

static constexpr uint64_t kOverlayVachunkFeatureFlags =
    VACHUNK_FEATURE_CU_GEOMETRY |
    VACHUNK_FEATURE_QP |
    VACHUNK_FEATURE_PRED_MODE |
    VACHUNK_FEATURE_MOTION_VECTORS |
    VACHUNK_FEATURE_REF_INDEXES |
    VACHUNK_FEATURE_BIT_COST;

static constexpr uint64_t kFrameSummaryExactFeatureFlags =
    VACHUNK_FEATURE_QP |
    VACHUNK_FEATURE_REF_INDEXES;
static constexpr uint64_t kFrameSummaryExactGeneratorRevision = 1;
static constexpr int32_t kFrameSummaryExactChunkFrames = 64;

static bool generate_ffmpeg_vachunk(const std::string& exe_dir,
                                    const std::string& data_dir,
                                    const std::string& staging_dir,
                                    const std::string& logs_dir,
                                    const char* video_path,
                                    const char* hash,
                                    AnalysisCodec codec,
                                    const std::string& vachunk_out,
                                    int32_t start_frame,
                                    int32_t end_frame,
                                    uint64_t base_content_revision,
                                    uint64_t generator_revision,
                                    const NativeCacheBudget& budget) {
    const char* codec_arg = ffmpeg_analysis_codec_arg(codec);
    if (!codec_arg) return false;

    const std::string analyzer_path = first_existing_tool_path({
        exe_dir + "\\tools\\ffmpeg-analysis\\void_ffmpeg_analyzer.exe",
        exe_dir + "\\tools\\ffmpeg-analysis\\void_hevc_analyzer.exe",
    });
    spdlog::info("[Analysis] ffmpeg-analysis producer={} exists={} codec={}",
                 analyzer_path.empty() ? "(none)" : analyzer_path,
                 !analyzer_path.empty(),
                 codec_arg);
    if (analyzer_path.empty()) return false;

    vr::win_utf8::delete_file_utf8(vachunk_out);

    const std::string analyzer_log_path = make_analysis_tool_log_path(
        logs_dir, "ffmpeg_analysis", hash);
    const std::string cmd = "\"" + analyzer_path +
        "\" --codec " + codec_arg + " --input \"" + video_path +
        "\" --vachunk \"" + vachunk_out +
        "\" --start-frame " + std::to_string(start_frame) +
        " --end-frame " + std::to_string(end_frame) +
        " --base-revision " + std::to_string(base_content_revision) +
        " --generator-revision " + std::to_string(generator_revision);
    spdlog::info("[Analysis] ffmpeg-analysis cmd: {}", cmd);
    spdlog::info("[Analysis] ffmpeg-analysis log: {}", analyzer_log_path);
    const uint64_t vachunk_budget = remaining_current_hash_budget(
        data_dir, hash, budget, staging_dir);
    if (budget.limited && vachunk_budget == 0) return false;
    const int analyzer_rc = run_command(
        cmd,
        analyzer_log_path,
        {vachunk_out},
        budget.limited ? vachunk_budget : 0);
    spdlog::info("[Analysis] ffmpeg-analysis exit_code={}", analyzer_rc);

    bool vachunk_ok = analyzer_rc == 0 && vr::win_utf8::file_exists_utf8(vachunk_out);
    if (vachunk_ok && !check_current_hash_budget(
            data_dir, hash, budget, "VACHUNK generation", staging_dir)) {
        vr::win_utf8::delete_file_utf8(vachunk_out);
        vachunk_ok = false;
    }
    spdlog::info("[Analysis] ffmpeg-analysis vachunk_out={} exists={}", vachunk_out, vachunk_ok);
    return vachunk_ok;
}

const vr::analysis::VachunkPayloadSection* find_payload_section(
    const vr::analysis::VachunkData& data,
    const char (&type)[5]) {
    for (const auto& section : data.sections) {
        if (std::memcmp(section.type, type, 4) == 0) {
            return &section;
        }
    }
    return nullptr;
}

static bool convert_overlay_fsum_to_exact(
    const vr::analysis::VachunkData& overlay_data,
    const vr::analysis::Vac2BaseFile& base,
    uint32_t start_frame,
    uint32_t end_frame,
    std::vector<Vac2FrameSummaryEntry>& exact) {
    exact.clear();
    const auto* fsum = find_payload_section(overlay_data, "FSUM");
    if (!fsum ||
        fsum->entry_size != sizeof(VachunkFrameSummary) ||
        fsum->decoded_size != fsum->bytes.size() ||
        fsum->entry_count != end_frame - start_frame + 1 ||
        static_cast<size_t>(end_frame) >= base.frames().size()) {
        return false;
    }

    const auto* source =
        reinterpret_cast<const VachunkFrameSummary*>(fsum->bytes.data());
    exact.resize(fsum->entry_count);
    for (uint32_t i = 0; i < fsum->entry_count; ++i) {
        const uint32_t frame_index = start_frame + i;
        const auto& src = source[i];
        auto& dst = exact[i];
        std::memset(&dst, 0, sizeof(dst));
        dst.poc = src.poc;
        dst.coded_order = frame_index;
        dst.first_vcl_unit = UINT32_MAX;
        if (static_cast<size_t>(frame_index) < base.frame_summaries().size()) {
            dst.first_vcl_unit = base.frame_summaries()[frame_index].first_vcl_unit;
        }
        dst.flags = VAC2_FRAME_SUMMARY_FLAG_EXACT_REFS;
        dst.temporal_id = src.temporal_id;
        dst.slice_type = src.slice_type;
        dst.nal_type = src.nal_unit_type;
        dst.qp_kind = src.num_cus > 0 ? VAC2_QP_KIND_EXACT : VAC2_QP_KIND_UNKNOWN;
        if (src.num_cus > 0) {
            dst.flags |= VAC2_FRAME_SUMMARY_FLAG_EXACT_QP;
            dst.qp_avg = src.avg_qp;
            dst.qp_min = src.qp_min;
            dst.qp_max = src.qp_max;
        }
        dst.num_ref_l0 = std::min<uint8_t>(src.num_ref_l0, 15);
        dst.num_ref_l1 = std::min<uint8_t>(src.num_ref_l1, 15);
        for (int ref = 0; ref < 15; ++ref) {
            dst.ref_pocs_l0[ref] = ref < dst.num_ref_l0 ? src.ref_pocs_l0[ref] : -1;
            dst.ref_pocs_l1[ref] = ref < dst.num_ref_l1 ? src.ref_pocs_l1[ref] : -1;
        }
    }
    return true;
}

static bool publish_frame_summary_exact_from_overlay(
    vr::analysis::VacacheStore& store,
    const vr::analysis::Vac2BaseFile& base,
    const vr::analysis::VachunkKey& key,
    const std::string& overlay_tmp,
    uint64_t max_output_bytes) {
    vr::analysis::VachunkData overlay_data;
    if (!vr::analysis::read_vachunk_file_data(overlay_tmp, overlay_data)) {
        spdlog::error("[Analysis] failed to read temporary overlay VACHUNK for FSUM");
        return false;
    }

    std::vector<Vac2FrameSummaryEntry> exact_summaries;
    if (!convert_overlay_fsum_to_exact(
            overlay_data, base, key.start_frame, key.end_frame, exact_summaries)) {
        spdlog::error("[Analysis] failed to convert overlay FSUM to exact frame summary");
        return false;
    }

    vr::analysis::VachunkData exact_data;
    exact_data.sections.push_back(
        vr::analysis::make_vachunk_string_section("META",
            "{\"producer\":\"analysis_ffi\",\"source\":\"ffmpeg-overlay-fsum\"}"));
    exact_data.sections.push_back(
        vr::analysis::make_vachunk_record_section("FSUM", exact_summaries));
    return store.write_chunk_atomic(key, std::move(exact_data), max_output_bytes);
}

static bool generate_frame_summary_exact_chunks(
    const std::string& exe_dir,
    const std::string& data_dir,
    const std::string& logs_dir,
    const char* video_path,
    const char* hash,
    vr::analysis::VacacheStore& store,
    const vr::analysis::Vac2BaseFile& base,
    int64_t max_cache_bytes) {
    const AnalysisCodec codec = analysis_codec_from_u16(base.header().codec);
    if (!ffmpeg_analysis_codec_arg(codec) || base.frames().empty()) {
        return true;
    }

    const NativeCacheBudget budget =
        compute_native_cache_budget(data_dir, hash, max_cache_bytes);
    if (budget.limited && budget.available_for_current_hash == 0) {
        spdlog::warn("[Analysis] skipping exact frame summaries; cache limit reached");
        return false;
    }

    const std::string staging_path = vr::win_utf8::path_to_utf8(
        vr::win_utf8::path_from_utf8(store.tmp_dir()) /
        vr::win_utf8::path_from_utf8(
            "summary." + std::to_string(GetCurrentProcessId()) + "." +
            std::to_string(GetTickCount64())));
    ScopedStagingDir staging(staging_path);
    if (!vr::win_utf8::create_directory_utf8(staging.path())) {
        spdlog::warn("[Analysis] failed to create exact summary staging dir");
        return false;
    }

    const auto frame_count = static_cast<uint32_t>(std::min<size_t>(
        base.frames().size(), std::numeric_limits<uint32_t>::max()));
    bool all_ok = true;
    for (uint32_t start = 0; start < frame_count;
         start += static_cast<uint32_t>(kFrameSummaryExactChunkFrames)) {
        const uint32_t end = std::min<uint32_t>(
            frame_count - 1, start + kFrameSummaryExactChunkFrames - 1);
        vr::analysis::VachunkKey key;
        key.kind = VachunkKind::FrameSummaryExact;
        key.codec = codec;
        key.feature_flags = kFrameSummaryExactFeatureFlags;
        key.base_content_revision = base.header().content_revision;
        key.generator_revision = kFrameSummaryExactGeneratorRevision;
        key.start_frame = start;
        key.end_frame = end;

        vr::analysis::VachunkFile existing;
        if (store.open_chunk(key, existing)) {
            existing.close();
            continue;
        }

        const std::string overlay_tmp =
            staging.path() + "\\" + std::string(hash) + ".summary." +
            std::to_string(start) + "." + std::to_string(end) + ".tmp.vck";
        const bool generated = generate_ffmpeg_vachunk(
            exe_dir, data_dir, staging.path(), logs_dir, video_path, hash,
            codec, overlay_tmp, static_cast<int32_t>(start), static_cast<int32_t>(end),
            key.base_content_revision, key.generator_revision, budget);
        if (!generated) {
            all_ok = false;
            vr::win_utf8::delete_file_utf8(overlay_tmp);
            continue;
        }

        const uint64_t max_output_bytes =
            overlay_publish_output_budget(data_dir, hash, budget);
        if (!publish_frame_summary_exact_from_overlay(
                store, base, key, overlay_tmp, max_output_bytes)) {
            all_ok = false;
        }
        vr::win_utf8::delete_file_utf8(overlay_tmp);
    }
    return all_ok;
}

static bool vachunk_matches_key(const vr::analysis::VachunkFile& chunk,
                                const vr::analysis::VachunkKey& key) {
    const auto& h = chunk.header();
    return h.kind == static_cast<uint16_t>(key.kind) &&
           h.codec == static_cast<uint16_t>(key.codec) &&
           h.feature_flags == key.feature_flags &&
           h.base_content_revision == key.base_content_revision &&
           h.generator_revision == key.generator_revision &&
           h.start_frame == key.start_frame &&
           h.end_frame == key.end_frame &&
           h.start_packet == key.start_packet &&
           h.end_packet == key.end_packet &&
           h.start_unit == key.start_unit &&
           h.end_unit == key.end_unit;
}

static bool publish_generated_vachunk(vr::analysis::VacacheStore& store,
                                      const vr::analysis::VachunkKey& key,
                                      const std::string& tmp_path,
                                      uint64_t max_output_bytes) {
    vr::analysis::VachunkFile verify;
    if (!verify.open(tmp_path) || !vachunk_matches_key(verify, key)) {
        spdlog::error("[Analysis] generated VACHUNK did not match requested key: {}", tmp_path);
        verify.close();
        return false;
    }
    verify.close();

    if (max_output_bytes > 0 && file_size_utf8(tmp_path) > max_output_bytes) {
        spdlog::error("[Analysis] generated VACHUNK exceeded output budget: {}", tmp_path);
        return false;
    }

    vr::analysis::VachunkData data;
    if (!vr::analysis::read_vachunk_file_data(tmp_path, data)) {
        spdlog::error("[Analysis] failed to read generated VACHUNK for publish: {}", tmp_path);
        return false;
    }
    if (!store.write_chunk_atomic(key, std::move(data), max_output_bytes)) {
        spdlog::error("[Analysis] failed to publish generated VACHUNK through VACache store");
        return false;
    }
    vr::win_utf8::delete_file_utf8(tmp_path);
    return true;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_generate_vac2_base(const char* video_path,
                                         const char* hash,
                                         const char* cache_root,
                                         int64_t max_cache_bytes) {
    std::lock_guard<std::mutex> lock(g_analysis_generate_mutex);
    if (!video_path || video_path[0] == '\0' || !hash || hash[0] == '\0' ||
        !cache_root || cache_root[0] == '\0') {
        spdlog::error("[Analysis] generate_vac2_base: video_path, hash, and cache_root must be non-empty");
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "video_path, hash, and cache_root are required");
        return 0;
    }

    vr::analysis::VacacheStore store(cache_root, hash);
    if (!store.ensure_layout()) {
        spdlog::error("[Analysis] generate_vac2_base: failed to create cache layout");
        set_analysis_error(NAKI_ANALYSIS_ERR_OPEN_FAILED,
                           "failed to create VAC2 cache layout");
        return 0;
    }

    std::ostringstream tmp_name;
    tmp_name << "base." << GetCurrentProcessId() << "." << GetTickCount64() << ".vac.tmp";
    const std::string tmp_path = vr::win_utf8::path_to_utf8(
        vr::win_utf8::path_from_utf8(store.tmp_dir()) /
        vr::win_utf8::path_from_utf8(tmp_name.str()));
    const uint64_t max_output_bytes =
        max_cache_bytes > 0 ? static_cast<uint64_t>(max_cache_bytes) : 0;

    spdlog::info("[Analysis] generate_vac2_base: video_path={}, hash={}, out={}",
                 video_path, hash, store.base_path());
    if (!vr::analysis::AnalysisGenerator::generate_vac2_base(
            video_path, tmp_path, max_output_bytes)) {
        spdlog::error("[Analysis] generate_vac2_base: generator failed");
        vr::win_utf8::delete_file_utf8(tmp_path);
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "failed to generate VAC2 base cache");
        return 0;
    }

    if (max_output_bytes > 0 && file_size_utf8(tmp_path) > max_output_bytes) {
        spdlog::warn("[Analysis] generate_vac2_base: output exceeded cache limit");
        vr::win_utf8::delete_file_utf8(tmp_path);
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "VAC2 base cache exceeded cache limit");
        return 0;
    }

    vr::win_utf8::delete_file_utf8(store.base_path());
    std::error_code rename_ec;
    std::filesystem::rename(
        vr::win_utf8::path_from_utf8(tmp_path),
        vr::win_utf8::path_from_utf8(store.base_path()),
        rename_ec);
    if (rename_ec) {
        spdlog::error("[Analysis] generate_vac2_base: publish failed: {} -> {} ({})",
                      tmp_path, store.base_path(), rename_ec.message());
        vr::win_utf8::delete_file_utf8(tmp_path);
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "failed to publish VAC2 base cache");
        return 0;
    }

    vr::analysis::Vac2BaseFile verify;
    if (!store.open_base(verify)) {
        spdlog::error("[Analysis] generate_vac2_base: published base failed to reopen");
        set_analysis_error(NAKI_ANALYSIS_ERR_OPEN_FAILED,
                           "published VAC2 base cache failed to reopen");
        return 0;
    }

    const std::string exe_dir = get_exe_dir();
    const std::string data_dir = cache_root;
    const std::string logs_dir =
        vr::win_utf8::path_to_utf8(vr::win_utf8::path_from_utf8(data_dir).parent_path() / L"logs");
    if (!generate_frame_summary_exact_chunks(
            exe_dir, data_dir, logs_dir, video_path, hash, store, verify,
            max_cache_bytes)) {
        spdlog::warn("[Analysis] generate_vac2_base: exact frame summaries unavailable");
    }

    set_analysis_ok();
    spdlog::info("[Analysis] generate_vac2_base succeeded");
    return 1;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_generate_vac2_overlay_chunk(const char* video_path,
                                                  const char* hash,
                                                  const char* cache_root,
                                                  int32_t start_frame,
                                                  int32_t end_frame,
                                                  int64_t max_cache_bytes) {
    std::lock_guard<std::mutex> lock(g_analysis_generate_mutex);
    if (!video_path || video_path[0] == '\0' || !hash || hash[0] == '\0' ||
        !cache_root || cache_root[0] == '\0' ||
        start_frame < 0 || end_frame < start_frame) {
        spdlog::error("[Analysis] generate_vac2_overlay_chunk: invalid arguments");
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "video_path, hash, cache_root, and a valid frame range are required");
        return 0;
    }

    vr::analysis::VacacheStore store(cache_root, hash);
    if (!store.ensure_layout()) {
        set_analysis_error(NAKI_ANALYSIS_ERR_OPEN_FAILED,
                           "failed to create VAC2 cache layout");
        return 0;
    }

    vr::analysis::Vac2BaseFile base;
    if (!store.open_base(base)) {
        spdlog::error("[Analysis] generate_vac2_overlay_chunk: base.vac is missing or invalid");
        set_analysis_error(NAKI_ANALYSIS_ERR_OPEN_FAILED,
                           "VAC2 base cache must exist before overlay chunks");
        return 0;
    }
    if (static_cast<uint32_t>(end_frame) >= base.frames().size()) {
        spdlog::error("[Analysis] generate_vac2_overlay_chunk: frame range {}-{} outside base frame count {}",
                      start_frame, end_frame, base.frames().size());
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "overlay frame range exceeds VAC2 base frame count");
        return 0;
    }

    const std::string exe_dir = get_exe_dir();
    const std::string data_dir = cache_root;
    const std::string logs_dir =
        vr::win_utf8::path_to_utf8(vr::win_utf8::path_from_utf8(data_dir).parent_path() / L"logs");
    const std::string staging_path = vr::win_utf8::path_to_utf8(
        vr::win_utf8::path_from_utf8(store.tmp_dir()) /
        vr::win_utf8::path_from_utf8(
            "overlay." + std::to_string(GetCurrentProcessId()) + "." +
            std::to_string(GetTickCount64())));
    ScopedStagingDir staging(staging_path);
    if (!vr::win_utf8::create_directory_utf8(staging.path())) {
        set_analysis_error(NAKI_ANALYSIS_ERR_OPEN_FAILED,
                           "failed to create overlay chunk staging directory");
        return 0;
    }

    const NativeCacheBudget budget =
        compute_native_cache_budget(data_dir, hash, max_cache_bytes);
    if (budget.limited && budget.available_for_current_hash == 0) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "cache limit leaves no room for overlay chunk generation");
        return 0;
    }

    const AnalysisCodec base_codec = analysis_codec_from_u16(base.header().codec);
    if (!is_supported_analysis_codec(base_codec)) {
        spdlog::error("[Analysis] generate_vac2_overlay_chunk: base codec {} is unsupported",
                      base.header().codec);
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                           "VAC2 base codec is unsupported");
        return 0;
    }

    const AnalysisCodec source_codec = detect_analysis_codec(video_path);
    if (source_codec != base_codec) {
        spdlog::warn("[Analysis] generate_vac2_overlay_chunk: detected codec {} differs from base codec {}",
                     static_cast<int>(source_codec), base.header().codec);
    }

    vr::analysis::VachunkKey key;
    key.kind = VachunkKind::Overlay;
    key.codec = base_codec;
    key.feature_flags = kOverlayVachunkFeatureFlags;
    key.base_content_revision = base.header().content_revision;
    key.generator_revision = 2;
    key.start_frame = static_cast<uint32_t>(start_frame);
    key.end_frame = static_cast<uint32_t>(end_frame);

    const std::string vachunk_tmp =
        staging.path() + "\\" + std::string(hash) + ".overlay.tmp.vck";
    bool vachunk_generated = false;
    if (ffmpeg_analysis_codec_arg(source_codec)) {
        vachunk_generated = generate_ffmpeg_vachunk(
            exe_dir, data_dir, staging.path(), logs_dir, video_path, hash,
            source_codec, vachunk_tmp, start_frame, end_frame,
            key.base_content_revision, key.generator_revision, budget);
    }
    if (!vachunk_generated || !vr::win_utf8::file_exists_utf8(vachunk_tmp)) {
        spdlog::error("[Analysis] generate_vac2_overlay_chunk: deep analyzer failed");
        vr::win_utf8::delete_file_utf8(vachunk_tmp);
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "failed to generate deep overlay analysis");
        return 0;
    }

    const uint64_t max_output_bytes =
        overlay_publish_output_budget(data_dir, hash, budget);
    if (budget.limited && max_output_bytes == 0) {
        vr::win_utf8::delete_file_utf8(vachunk_tmp);
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "cache limit leaves no room to publish overlay chunk");
        return 0;
    }
    if (!publish_generated_vachunk(store, key, vachunk_tmp, max_output_bytes)) {
        vr::win_utf8::delete_file_utf8(vachunk_tmp);
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "failed to publish overlay VACHUNK");
        return 0;
    }

    set_analysis_ok();
    spdlog::info("[Analysis] generate_vac2_overlay_chunk succeeded: hash={}, frames={}-{}",
                 hash, start_frame, end_frame);
    return 1;
}
