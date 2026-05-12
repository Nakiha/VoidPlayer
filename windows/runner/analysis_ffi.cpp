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
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

// Callback registered by video_renderer_plugin to provide current PTS.
// Avoids analysis_ffi needing to know about vr::Renderer.
using PtsCallback = int64_t (*)();
static std::atomic<PtsCallback> g_get_current_pts_us{nullptr};

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

void naki_analysis_register_pts_callback(int64_t (*cb)()) {
    g_get_current_pts_us.store(cb, std::memory_order_release);
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
    vr::analysis::AnalysisManager manager;
    std::unique_ptr<vr::analysis::Vac2BaseFile> vac2_base;
    std::mutex mutex;
    bool closed = false;

    bool is_vac2() const { return static_cast<bool>(vac2_base); }
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

bool open_analysis_handle_path(AnalysisHandleState& state, const char* analysis_path) {
    if (is_vac2_base_path(analysis_path)) {
        auto base = std::make_unique<vr::analysis::Vac2BaseFile>();
        if (!base->open(analysis_path)) return false;
        state.vac2_base = std::move(base);
        state.manager.unload();
        return true;
    }

    state.vac2_base.reset();
    return state.manager.load(analysis_path);
}

int32_t clamp_count_to_i32(size_t count) {
    return count > static_cast<size_t>(std::numeric_limits<int32_t>::max())
        ? std::numeric_limits<int32_t>::max()
        : static_cast<int32_t>(count);
}

int effective_frame_count(vr::analysis::AnalysisManager& mgr);
bool fill_vac2_frame_at(const vr::analysis::Vac2BaseFile& base,
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

void fill_analysis_summary(vr::analysis::AnalysisManager& mgr, NakiAnalysisSummary& s) {
    std::memset(&s, 0, sizeof(s));
    s.current_frame_idx = -1;
    if (!mgr.is_loaded()) return;

    s.loaded = 1;
    const auto& base = mgr.vac2_base();
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

    if (auto cb = g_get_current_pts_us.load(std::memory_order_acquire)) {
        int64_t pts_us = cb();
        s.current_frame_idx = mgr.current_frame_idx(pts_us);
    }
}

int effective_frame_count(vr::analysis::AnalysisManager& mgr) {
    return mgr.is_loaded() ? mgr.frame_count() : 0;
}

int32_t fill_analysis_frames_range(vr::analysis::AnalysisManager& mgr,
                                   int32_t start,
                                   NakiFrameInfo* out,
                                   int32_t max_count);

bool fill_analysis_frame_at(vr::analysis::AnalysisManager& mgr,
                            int32_t source_index,
                            NakiFrameInfo& f);

int32_t fill_analysis_frames(vr::analysis::AnalysisManager& mgr,
                             NakiFrameInfo* out,
                             int32_t max_count) {
    return fill_analysis_frames_range(mgr, 0, out, max_count);
}

int32_t fill_analysis_frames_range(vr::analysis::AnalysisManager& mgr,
                                   int32_t start,
                                   NakiFrameInfo* out,
                                   int32_t max_count) {
    if (!out || max_count <= 0) return 0;
    if (!mgr.is_loaded()) return 0;
    if (start < 0) return 0;

    int total_count = effective_frame_count(mgr);
    if (start >= total_count) return 0;
    int count = std::min(max_count, total_count - start);

    for (int i = 0; i < count; i++) {
        if (!fill_analysis_frame_at(mgr, start + i, out[i])) {
            return i;
        }
    }
    return count;
}

bool fill_analysis_frame_at(vr::analysis::AnalysisManager& mgr,
                            int32_t source_index,
                            NakiFrameInfo& f) {
    if (!mgr.is_loaded()) return false;
    return fill_vac2_frame_at(mgr.vac2_base(), source_index, f);
}

int32_t fill_analysis_nalus_range(vr::analysis::AnalysisManager& mgr,
                                  int32_t start,
                                  NakiNaluInfo* out,
                                  int32_t max_count);

int32_t fill_analysis_nalus(vr::analysis::AnalysisManager& mgr,
                            NakiNaluInfo* out,
                            int32_t max_count) {
    return fill_analysis_nalus_range(mgr, 0, out, max_count);
}

int32_t fill_analysis_nalus_range(vr::analysis::AnalysisManager& mgr,
                                  int32_t start,
                                  NakiNaluInfo* out,
                                  int32_t max_count) {
    if (!out || max_count <= 0) return 0;
    if (!mgr.is_loaded()) return 0;
    if (start < 0) return 0;

    return fill_vac2_nalus_range(mgr.vac2_base(), start, out, max_count);
}

int32_t frame_to_nalu(vr::analysis::AnalysisManager& mgr, int32_t frame_index) {
    if (!mgr.is_loaded()) return -1;
    return vac2_frame_to_nalu(mgr.vac2_base(), frame_index);
}

int32_t nalu_to_frame(vr::analysis::AnalysisManager& mgr, int32_t nalu_index) {
    if (!mgr.is_loaded()) return -1;
    return vac2_nalu_to_frame(mgr.vac2_base(), nalu_index);
}

int32_t fill_analysis_frame_buckets(vr::analysis::AnalysisManager& mgr,
                                    int32_t start,
                                    int32_t bucket_size,
                                    NakiFrameBucket* out,
                                    int32_t max_count) {
    if (!mgr.is_loaded()) return 0;
    return fill_vac2_frame_buckets(mgr.vac2_base(), start, bucket_size, out, max_count);
}

int32_t current_vac2_frame_idx(const vr::analysis::Vac2BaseFile& base) {
    const auto& frames = base.frames();
    if (frames.empty()) return -1;
    auto cb = g_get_current_pts_us.load(std::memory_order_acquire);
    if (!cb) return -1;

    const auto& h = base.header();
    if (h.time_base_num <= 0 || h.time_base_den <= 0) return -1;
    const int64_t pts_us = cb();
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
                        int32_t source_index,
                        NakiFrameInfo& f) {
    if (source_index < 0 ||
        static_cast<size_t>(source_index) >= base.frames().size()) {
        return false;
    }

    const auto& frame = base.frames()[source_index];
    const Vac2FrameSummaryEntry* summary = nullptr;
    if (static_cast<size_t>(source_index) < base.frame_summaries().size()) {
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
                               int32_t start,
                               NakiFrameInfo* out,
                               int32_t max_count) {
    if (!out || max_count <= 0 || start < 0) return 0;
    const int32_t total_count = clamp_count_to_i32(base.frames().size());
    if (start >= total_count) return 0;
    const int32_t count = std::min(max_count, total_count - start);
    for (int32_t i = 0; i < count; ++i) {
        if (!fill_vac2_frame_at(base, start + i, out[i])) return i;
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
            if (!fill_vac2_frame_at(base, bucket_start + i, f)) break;
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
int32_t naki_analysis_load(const char* analysis_path) {
    if (!analysis_path || analysis_path[0] == '\0') {
        set_analysis_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "analysis_path is required");
        return 0;
    }
    static int load_count = 0;
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    LogStackUsage(fmt::format("analysis_load #{}", ++load_count).c_str());
    auto& mgr = vr::analysis::AnalysisManager::instance();
    if (!mgr.load(analysis_path)) {
        set_analysis_error(NAKI_ANALYSIS_ERR_OPEN_FAILED, "failed to load analysis container");
        return 0;
    }
    set_analysis_ok();
    return 1;
}

extern "C" __declspec(dllexport)
void naki_analysis_unload() {
    static int unload_count = 0;
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    LogStackUsage(fmt::format("analysis_unload #{}", ++unload_count).c_str());
    vr::analysis::AnalysisManager::instance().unload();
    set_analysis_ok();
}

extern "C" __declspec(dllexport)
const NakiAnalysisSummary* naki_analysis_get_summary() {
    thread_local NakiAnalysisSummary s{};
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& mgr = vr::analysis::AnalysisManager::instance();
    fill_analysis_summary(mgr, s);
    return &s;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frames(NakiFrameInfo* out, int32_t max_count) {
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return fill_analysis_frames(mgr, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frames_range(int32_t start, NakiFrameInfo* out, int32_t max_count) {
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return fill_analysis_frames_range(mgr, start, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_nalus(NakiNaluInfo* out, int32_t max_count) {
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return fill_analysis_nalus(mgr, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_nalus_range(int32_t start, NakiNaluInfo* out, int32_t max_count) {
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return fill_analysis_nalus_range(mgr, start, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_frame_to_nalu(int32_t frame_index) {
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return frame_to_nalu(mgr, frame_index);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_nalu_to_frame(int32_t nalu_index) {
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return nalu_to_frame(mgr, nalu_index);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frame_buckets(int32_t start, int32_t bucket_size, NakiFrameBucket* out, int32_t max_count) {
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return fill_analysis_frame_buckets(mgr, start, bucket_size, out, max_count);
}

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
    overlay.opacity_permille.store(std::clamp(state->opacity_permille, 100, 1000), std::memory_order_release);
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
            state->manager.unload();
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
    state->manager.unload();
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
        if (state->is_vac2()) {
            fill_vac2_summary(*state->vac2_base, summary);
        } else {
            fill_analysis_summary(state->manager, summary);
        }
        set_analysis_ok();
    }
    return &summary;
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
    auto count = state->is_vac2()
        ? fill_vac2_frames_range(*state->vac2_base, 0, out, max_count)
        : fill_analysis_frames(state->manager, out, max_count);
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
    auto count = state->is_vac2()
        ? fill_vac2_frames_range(*state->vac2_base, start, out, max_count)
        : fill_analysis_frames_range(state->manager, start, out, max_count);
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
    auto count = state->is_vac2()
        ? fill_vac2_nalus_range(*state->vac2_base, 0, out, max_count)
        : fill_analysis_nalus(state->manager, out, max_count);
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
    auto count = state->is_vac2()
        ? fill_vac2_nalus_range(*state->vac2_base, start, out, max_count)
        : fill_analysis_nalus_range(state->manager, start, out, max_count);
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
    auto index = state->is_vac2()
        ? vac2_frame_to_nalu(*state->vac2_base, frame_index)
        : frame_to_nalu(state->manager, frame_index);
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
    auto index = state->is_vac2()
        ? vac2_nalu_to_frame(*state->vac2_base, nalu_index)
        : nalu_to_frame(state->manager, nalu_index);
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
    auto count = state->is_vac2()
        ? fill_vac2_frame_buckets(*state->vac2_base, start, bucket_size, out, max_count)
        : fill_analysis_frame_buckets(state->manager, start, bucket_size, out, max_count);
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
        ends_with_ascii(name, ".tmp.vbs4") ||
        ends_with_ascii(name, ".tmp.vbi") ||
        ends_with_ascii(name, ".tmp.vbt") ||
        ends_with_ascii(name, ".tmp.vvc") ||
        ends_with_ascii(name, ".vbs4") ||
        ends_with_ascii(name, ".vbi") ||
        ends_with_ascii(name, ".vbt") ||
        ends_with_ascii(name, ".vbs2");
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
        ".vac", ".tmp.vbs4", ".tmp.vbi", ".tmp.vbt", ".tmp.vvc",
        ".vbs4", ".vbi", ".vbt", ".vbs2",
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

static VbiCodec detect_analysis_codec(const char* video_path) {
    if (vr::PrivateCdnFlvDemuxer::probe(video_path)) {
        vr::PrivateCdnFlvDemuxer demuxer;
        if (demuxer.open(video_path) && demuxer.stats().codec_params) {
            VbiCodec codec = vr::analysis::BitstreamIndexer::codec_from_ffmpeg_id(
                demuxer.stats().codec_params->codec_id);
            if (codec != VbiCodec::Unknown) return codec;
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
                    VbiCodec codec = vr::analysis::BitstreamIndexer::codec_from_ffmpeg_id(
                        codecpar->codec_id);
                    avformat_close_input(&fmt_ctx);
                    if (codec != VbiCodec::Unknown) return codec;
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

// RAII helper: temporarily set env vars, restore on destruction.
struct ScopedEnvVars {
    std::vector<std::pair<std::string, std::string>> saved;

    void set(const char* name, const std::string& value) {
        auto wide_name = vr::win_utf8::utf16_from_utf8(name);
        saved.emplace_back(name, vr::win_utf8::get_env_utf8(wide_name.c_str()));
        vr::win_utf8::set_env_utf8(wide_name.c_str(), value);
    }

    ~ScopedEnvVars() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            auto wide_name = vr::win_utf8::utf16_from_utf8(it->first);
            if (it->second.empty()) {
                SetEnvironmentVariableW(wide_name.c_str(), nullptr);
            } else {
                vr::win_utf8::set_env_utf8(wide_name.c_str(), it->second);
            }
        }
    }
};

class RawVvcSink {
public:
    virtual ~RawVvcSink() = default;
    virtual bool write(const uint8_t* data, size_t size) = 0;
    virtual bool finish() { return true; }
};

class FileRawVvcSink : public RawVvcSink {
public:
    explicit FileRawVvcSink(const std::string& path, uint64_t max_bytes = 0)
        : path_(path),
          max_bytes_(max_bytes),
          out_(vr::win_utf8::path_from_utf8(path), std::ios::binary) {}

    bool is_open() const { return out_.is_open(); }

    bool write(const uint8_t* data, size_t size) override {
        if (max_bytes_ > 0 && size > max_bytes_ - bytes_written_) {
            spdlog::warn("[Analysis] raw VVC output exceeded cache limit");
            return false;
        }
        out_.write(reinterpret_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
        if (!out_.good()) return false;
        bytes_written_ += static_cast<uint64_t>(size);
        return true;
    }

    bool finish() override {
        out_.close();
        return !out_.fail();
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
    uint64_t max_bytes_ = 0;
    uint64_t bytes_written_ = 0;
    std::ofstream out_;
};

class HandleRawVvcSink : public RawVvcSink {
public:
    explicit HandleRawVvcSink(HANDLE handle) : handle_(handle) {}

    bool write(const uint8_t* data, size_t size) override {
        size_t offset = 0;
        while (offset < size) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(size - offset, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(handle_, data + offset, chunk, &written, nullptr)) {
                spdlog::warn("[Analysis] write to VTM stdin failed: error={}",
                             GetLastError());
                return false;
            }
            if (written == 0) {
                return false;
            }
            offset += written;
        }
        return true;
    }

private:
    HANDLE handle_;
};

static bool write_annex_b_packet_payload(const uint8_t* data,
                                         int len,
                                         RawVvcSink& sink,
                                         size_t& total_written) {
    static const uint8_t kStartCode4[] = {0, 0, 0, 1};
    if (!data || len <= 0) return true;

    if ((len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
        (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)) {
        if (!sink.write(data, static_cast<size_t>(len))) {
            return false;
        }
        total_written += static_cast<size_t>(len);
        return true;
    }

    int pos = 0;
    bool wrote_any = false;
    while (pos + 4 <= len) {
        uint32_t nalu_len = (static_cast<uint32_t>(data[pos]) << 24) |
                            (static_cast<uint32_t>(data[pos + 1]) << 16) |
                            (static_cast<uint32_t>(data[pos + 2]) << 8) |
                            static_cast<uint32_t>(data[pos + 3]);
        const int payload_pos = pos + 4;
        const int remaining = len - payload_pos;
        if (nalu_len == 0 ||
            static_cast<uint64_t>(nalu_len) > static_cast<uint64_t>(remaining)) {
            break;
        }
        if (!sink.write(kStartCode4, 4) ||
            !sink.write(data + payload_pos, static_cast<size_t>(nalu_len))) {
            return false;
        }
        total_written += 4 + static_cast<size_t>(nalu_len);
        wrote_any = true;
        pos = payload_pos + static_cast<int>(nalu_len);
    }
    return wrote_any;
}

static bool extract_private_cdn_flv_annex_b_to_sink(const std::string& video_path,
                                                    const char* bsf_name,
                                                    const char* log_label,
                                                    RawVvcSink& sink) {
    vr::PrivateCdnFlvDemuxer demuxer;
    if (!demuxer.open(video_path)) {
        return false;
    }
    const auto& stats = demuxer.stats();
    if (!stats.codec_params || stats.video_stream_index < 0) {
        return false;
    }

    AVBSFContext* bsf_ctx = nullptr;
    bool use_bsf = false;
    const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
    if (bsf && av_bsf_alloc(bsf, &bsf_ctx) >= 0) {
        int ret = avcodec_parameters_copy(bsf_ctx->par_in, stats.codec_params);
        if (ret >= 0) {
            bsf_ctx->time_base_in = stats.time_base;
            ret = av_bsf_init(bsf_ctx);
        }
        if (ret >= 0) {
            use_bsf = true;
            spdlog::info("[Analysis] {}: using private CDN FLV demuxer with {} BSF",
                         log_label, bsf_name);
        } else {
            spdlog::warn("[Analysis] {}: private CDN FLV BSF init failed: {:#x}",
                         log_label, static_cast<unsigned>(ret));
            av_bsf_free(&bsf_ctx);
        }
    }

    AVPacket* pkt = av_packet_alloc();
    AVPacket* filtered_pkt = av_packet_alloc();
    if (!pkt || !filtered_pkt) {
        av_packet_free(&filtered_pkt);
        av_packet_free(&pkt);
        if (bsf_ctx) av_bsf_free(&bsf_ctx);
        return false;
    }

    size_t total_written = 0;
    int pkt_count = 0;
    while (true) {
        int ret = demuxer.read_packet(pkt);
        if (ret < 0) {
            if (ret != AVERROR_EOF) {
                spdlog::warn("[Analysis] {}: private CDN FLV read failed: {:#x}",
                             log_label, static_cast<unsigned>(ret));
            }
            break;
        }
        if (pkt->stream_index != stats.video_stream_index) {
            av_packet_unref(pkt);
            continue;
        }
        pkt_count++;

        if (use_bsf) {
            ret = av_bsf_send_packet(bsf_ctx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) continue;
            while (av_bsf_receive_packet(bsf_ctx, filtered_pkt) == 0) {
                if (!sink.write(filtered_pkt->data, static_cast<size_t>(filtered_pkt->size))) {
                    av_packet_unref(filtered_pkt);
                    av_packet_free(&filtered_pkt);
                    av_packet_free(&pkt);
                    av_bsf_free(&bsf_ctx);
                    return false;
                }
                total_written += static_cast<size_t>(filtered_pkt->size);
                av_packet_unref(filtered_pkt);
            }
        } else {
            const bool ok = write_annex_b_packet_payload(
                pkt->data, pkt->size, sink, total_written);
            av_packet_unref(pkt);
            if (!ok) {
                av_packet_free(&filtered_pkt);
                av_packet_free(&pkt);
                if (bsf_ctx) av_bsf_free(&bsf_ctx);
                return false;
            }
        }
    }

    if (use_bsf) {
        av_bsf_send_packet(bsf_ctx, nullptr);
        while (av_bsf_receive_packet(bsf_ctx, filtered_pkt) == 0) {
            if (!sink.write(filtered_pkt->data, static_cast<size_t>(filtered_pkt->size))) {
                av_packet_unref(filtered_pkt);
                av_packet_free(&filtered_pkt);
                av_packet_free(&pkt);
                av_bsf_free(&bsf_ctx);
                return false;
            }
            total_written += static_cast<size_t>(filtered_pkt->size);
            av_packet_unref(filtered_pkt);
        }
    }

    av_packet_free(&filtered_pkt);
    av_packet_free(&pkt);
    if (bsf_ctx) av_bsf_free(&bsf_ctx);

    const bool finished = sink.finish();
    spdlog::info("[Analysis] {}: private CDN FLV {} packets, {} bytes written",
                 log_label, pkt_count, total_written);
    return total_written > 0 && finished;
}

// Extract a raw Annex B bitstream from a container using FFmpeg C API.
// H.264/HEVC/VVC MP4 samples are length-prefixed, so use the matching
// mp4toannexb BSF when available and fall back to a conservative manual path.
static bool extract_raw_annex_b_to_sink(const std::string& video_path,
                                        const char* bsf_name,
                                        const char* log_label,
                                        RawVvcSink& sink) {
    if (vr::PrivateCdnFlvDemuxer::probe(video_path)) {
        return extract_private_cdn_flv_annex_b_to_sink(video_path, bsf_name, log_label, sink);
    }

    FfmpegOpenTimeout timeout;
    AVFormatContext* fmt_ctx = alloc_format_context_with_timeout(timeout, std::chrono::seconds(30));
    if (!fmt_ctx) {
        spdlog::error("[Analysis] {}: failed to allocate format context", log_label);
        return false;
    }
    int ret = avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        timeout.deadline_ns = 0;
        spdlog::error("[Analysis] {}: open failed: {:#x}", log_label, static_cast<unsigned>(ret));
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    timeout.deadline_ns = 0;
    if (ret < 0) {
        spdlog::error("[Analysis] {}: find_stream_info failed: {:#x}", log_label, static_cast<unsigned>(ret));
        avformat_close_input(&fmt_ctx);
        return false;
    }

    // Find video stream
    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }
    if (video_idx < 0) {
        spdlog::error("[Analysis] {}: no video stream found", log_label);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    AVBSFContext* bsf_ctx = nullptr;
    bool use_bsf = false;
    const AVBitStreamFilter* bsf = av_bsf_get_by_name(bsf_name);
    if (bsf) {
        ret = av_bsf_alloc(bsf, &bsf_ctx);
        if (ret >= 0) {
            avcodec_parameters_copy(bsf_ctx->par_in, fmt_ctx->streams[video_idx]->codecpar);
            ret = av_bsf_init(bsf_ctx);
            if (ret >= 0) {
                use_bsf = true;
                spdlog::info("[Analysis] {}: using {} BSF", log_label, bsf_name);
            } else {
                spdlog::warn("[Analysis] {}: BSF init failed: {:#x}, falling back to manual",
                             log_label, static_cast<unsigned>(ret));
                av_bsf_free(&bsf_ctx);
            }
        }
    } else {
        spdlog::warn("[Analysis] {}: {} BSF not found, using manual conversion", log_label, bsf_name);
    }

    static const uint8_t kStartCode4[] = {0, 0, 0, 1};
    size_t total_written = 0;
    int pkt_count = 0;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        spdlog::error("[Analysis] {}: failed to allocate packet", log_label);
        if (bsf_ctx) av_bsf_free(&bsf_ctx);
        avformat_close_input(&fmt_ctx);
        return false;
    }
    while (true) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) break;
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }
        pkt_count++;

        if (use_bsf) {
            ret = av_bsf_send_packet(bsf_ctx, pkt);
            if (ret < 0) {
                av_packet_unref(pkt);
                continue;
            }
            while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
                if (!sink.write(pkt->data, static_cast<size_t>(pkt->size))) {
                    av_packet_unref(pkt);
                    av_packet_free(&pkt);
                    av_bsf_free(&bsf_ctx);
                    avformat_close_input(&fmt_ctx);
                    return false;
                }
                total_written += static_cast<size_t>(pkt->size);
                av_packet_unref(pkt);
            }
        } else {
            const uint8_t* data = pkt->data;
            int len = pkt->size;

            if ((len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
                (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)) {
                if (!sink.write(data, static_cast<size_t>(len))) {
                    av_packet_unref(pkt);
                    av_packet_free(&pkt);
                    if (bsf_ctx) av_bsf_free(&bsf_ctx);
                    avformat_close_input(&fmt_ctx);
                    return false;
                }
                total_written += static_cast<size_t>(len);
            } else {
                int pos = 0;
                while (pos + 4 <= len) {
                    uint32_t nalu_len = (static_cast<uint32_t>(data[pos]) << 24) |
                                        (static_cast<uint32_t>(data[pos + 1]) << 16) |
                                        (static_cast<uint32_t>(data[pos + 2]) << 8) |
                                        static_cast<uint32_t>(data[pos + 3]);
                    const int payload_pos = pos + 4;
                    const int remaining = len - payload_pos;
                    if (nalu_len == 0 ||
                        static_cast<uint64_t>(nalu_len) > static_cast<uint64_t>(remaining)) {
                        break;
                    }
                    if (!sink.write(kStartCode4, 4) ||
                        !sink.write(data + payload_pos, static_cast<size_t>(nalu_len))) {
                        av_packet_unref(pkt);
                        av_packet_free(&pkt);
                        if (bsf_ctx) av_bsf_free(&bsf_ctx);
                        avformat_close_input(&fmt_ctx);
                        return false;
                    }
                    total_written += 4 + static_cast<size_t>(nalu_len);
                    pos = payload_pos + static_cast<int>(nalu_len);
                }
            }
            av_packet_unref(pkt);
        }
    }

    // Flush BSF
    if (use_bsf) {
        av_bsf_send_packet(bsf_ctx, nullptr);
        while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
            if (!sink.write(pkt->data, static_cast<size_t>(pkt->size))) {
                av_packet_unref(pkt);
                av_packet_free(&pkt);
                av_bsf_free(&bsf_ctx);
                avformat_close_input(&fmt_ctx);
                return false;
            }
            total_written += static_cast<size_t>(pkt->size);
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
    if (bsf_ctx) av_bsf_free(&bsf_ctx);
    avformat_close_input(&fmt_ctx);

    const bool finished = sink.finish();
    spdlog::info("[Analysis] {}: {} packets, {} bytes written",
                 log_label, pkt_count, total_written);
    return total_written > 0 && finished;
}

static bool extract_raw_vvc_to_sink(const std::string& video_path, RawVvcSink& sink) {
    return extract_raw_annex_b_to_sink(
        video_path, "vvc_mp4toannexb", "extract_raw_vvc", sink);
}

static bool extract_raw_vvc(const std::string& video_path,
                            const std::string& out_path,
                            uint64_t max_output_bytes = 0) {
    FileRawVvcSink sink(out_path, max_output_bytes);
    if (!sink.is_open()) {
        spdlog::error("[Analysis] extract_raw_vvc: cannot create {}", out_path);
        return false;
    }

    bool ok = extract_raw_vvc_to_sink(video_path, sink);
    if (!ok) {
        spdlog::info("[Analysis] extract_raw_vvc: FFmpeg produced no packets, trying raw Annex-B fallback");
        if (vr::analysis::BitstreamIndexer::write_annex_b_file(
                video_path, VbiCodec::VVC, out_path)) {
            std::ifstream fallback(vr::win_utf8::path_from_utf8(out_path),
                                   std::ios::binary | std::ios::ate);
            const size_t total_written = fallback ? static_cast<size_t>(fallback.tellg()) : 0;
            ok = total_written > 0 &&
                (max_output_bytes == 0 ||
                 static_cast<uint64_t>(total_written) <= max_output_bytes);
            if (!ok && max_output_bytes > 0) {
                vr::win_utf8::delete_file_utf8(out_path);
            }
        }
    }
    return ok;
}

static int run_vtm_stdin_command(const std::string& cmd,
                                 const std::string& log_path,
                                 const std::string& video_path,
                                 const std::vector<std::string>& watched_paths = {},
                                 uint64_t max_watched_bytes = 0) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };

    HANDLE hLogFile = INVALID_HANDLE_VALUE;
    if (!log_path.empty()) {
        const auto wide_log_path = vr::win_utf8::utf16_from_utf8(log_path);
        hLogFile = CreateFileW(wide_log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);
        return -1;
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = stdin_read;
    si.hStdOutput = hLogFile != INVALID_HANDLE_VALUE
        ? hLogFile
        : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = hLogFile != INVALID_HANDLE_VALUE
        ? hLogFile
        : GetStdHandle(STD_ERROR_HANDLE);
    si.wShowWindow = SW_HIDE;

    std::wstring cmdline = vr::win_utf8::utf16_from_utf8(cmd);
    if (cmdline.empty()) {
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);
        return -1;
    }

    if (!CreateProcessW(
            nullptr, cmdline.data(),
            nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi)) {
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);
        return -1;
    }

    CloseHandle(stdin_read);

    HandleRawVvcSink sink(stdin_write);
    const bool wrote = extract_raw_vvc_to_sink(video_path, sink);
    CloseHandle(stdin_write);

    if (!wrote) {
        spdlog::warn("[Analysis] VTM stdin feed failed, terminating DecoderApp");
        TerminateProcess(pi.hProcess, 1);
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
    if (killed_for_limit) return -2;
    return wrote ? static_cast<int>(exit_code) : -1;
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

static bool generate_vvc_vbs4(const std::string& exe_dir,
                              const std::string& data_dir,
                              const std::string& staging_dir,
                              const std::string& logs_dir,
                              const char* video_path,
                              const char* hash,
                              const std::string& vbs4_out,
                              const NativeCacheBudget& budget) {
    const std::string decoder_path = exe_dir + "\\tools\\vtm\\DecoderApp.exe";
    const bool decoder_exists = vr::win_utf8::file_exists_utf8(decoder_path);
    spdlog::info("[Analysis] vvc producer={} exists={}", decoder_path, decoder_exists);
    if (!decoder_exists) return false;

    vr::win_utf8::delete_file_utf8(vbs4_out);

    ScopedEnvVars env;
    env.set("VTM_BINARY_STATS", vbs4_out);
    env.set("VOID_VTM_STDIN_WINDOW_BYTES", "67108864");
    env.set("VOID_VTM_STDIN_WINDOW_NALUS", "4096");
    env.set("VOID_VTM_STDIN_HARD_CAP_BYTES", "268435456");

    const std::string vtm_log_path = make_analysis_tool_log_path(logs_dir, "vtm", hash);
    std::string cmd = "\"" + decoder_path +
        "\" -b - --TraceFile=NUL --TraceRule=\"D_BLOCK_STATISTICS_CODED:poc>=0\" -o NUL";
    spdlog::info("[Analysis] vtm stdin cmd: {}", cmd);
    spdlog::info("[Analysis] vtm log: {}", vtm_log_path);
    const uint64_t stdin_budget = remaining_current_hash_budget(
        data_dir, hash, budget, staging_dir);
    if (budget.limited && stdin_budget == 0) return false;
    const int vtm_rc = run_vtm_stdin_command(
        cmd,
        vtm_log_path,
        video_path,
        {vbs4_out},
        budget.limited ? stdin_budget : 0);
    spdlog::info("[Analysis] vtm stdin exit_code={}", vtm_rc);

    bool vbs4_ok = vtm_rc == 0 && vr::win_utf8::file_exists_utf8(vbs4_out);
    if (!vbs4_ok) {
        spdlog::warn("[Analysis] VTM stdin generation failed, falling back to temp VVC file");
        vr::win_utf8::delete_file_utf8(vbs4_out);

        const std::string tmp_vvc = staging_dir + "\\" + hash + ".tmp.vvc";
        spdlog::info("[Analysis] extracting raw VVC to {}", tmp_vvc);
        const uint64_t extract_budget = remaining_current_hash_budget(
            data_dir, hash, budget, staging_dir);
        if (budget.limited && extract_budget == 0) {
            vr::win_utf8::delete_file_utf8(tmp_vvc);
            return false;
        }
        const bool demux_ok = extract_raw_vvc(
            video_path,
            tmp_vvc,
            budget.limited ? extract_budget : 0);
        spdlog::info("[Analysis] extract_raw_vvc ok={}", demux_ok);

        if (demux_ok &&
            vr::win_utf8::file_exists_utf8(tmp_vvc) &&
            check_current_hash_budget(
                data_dir, hash, budget, "raw VVC extraction", staging_dir)) {
            const std::string fallback_cmd = "\"" + decoder_path + "\" -b \"" + tmp_vvc +
                "\" --TraceFile=NUL --TraceRule=\"D_BLOCK_STATISTICS_CODED:poc>=0\" -o NUL";
            spdlog::info("[Analysis] vtm fallback cmd: {}", fallback_cmd);
            const int fallback_rc = run_command(
                fallback_cmd,
                vtm_log_path,
                {tmp_vvc, vbs4_out},
                budget.limited ? remaining_current_hash_budget(
                    data_dir, hash, budget, staging_dir) : 0);
            spdlog::info("[Analysis] vtm fallback exit_code={}", fallback_rc);
            vr::win_utf8::delete_file_utf8(tmp_vvc);
            vbs4_ok = fallback_rc == 0 && vr::win_utf8::file_exists_utf8(vbs4_out);
        } else {
            spdlog::warn("[Analysis] raw VVC extraction failed, skipping VBS4 generation");
            vr::win_utf8::delete_file_utf8(tmp_vvc);
        }
    }

    if (vbs4_ok && !check_current_hash_budget(
            data_dir, hash, budget, "VBS4 generation", staging_dir)) {
        vr::win_utf8::delete_file_utf8(vbs4_out);
        vbs4_ok = false;
    }

    spdlog::info("[Analysis] vvc vbs4_out={} exists={}", vbs4_out, vbs4_ok);
    return vbs4_ok;
}

static const char* ffmpeg_analysis_codec_arg(VbiCodec codec) {
    switch (codec) {
    case VbiCodec::HEVC:  return "hevc";
    case VbiCodec::H264:  return "h264";
    default:              return nullptr;
    }
}

static bool generate_ffmpeg_vbs4(const std::string& exe_dir,
                                 const std::string& data_dir,
                                 const std::string& staging_dir,
                                 const std::string& logs_dir,
                                 const char* video_path,
                                 const char* hash,
                                 VbiCodec codec,
                                 const std::string& vbs4_out,
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

    vr::win_utf8::delete_file_utf8(vbs4_out);

    const std::string analyzer_log_path = make_analysis_tool_log_path(
        logs_dir, "ffmpeg_analysis", hash);
    const std::string cmd = "\"" + analyzer_path +
        "\" --codec " + codec_arg + " --input \"" + video_path +
        "\" --vbs4 \"" + vbs4_out + "\"";
    spdlog::info("[Analysis] ffmpeg-analysis cmd: {}", cmd);
    spdlog::info("[Analysis] ffmpeg-analysis log: {}", analyzer_log_path);
    const uint64_t vbs4_budget = remaining_current_hash_budget(
        data_dir, hash, budget, staging_dir);
    if (budget.limited && vbs4_budget == 0) return false;
    const int analyzer_rc = run_command(
        cmd,
        analyzer_log_path,
        {vbs4_out},
        budget.limited ? vbs4_budget : 0);
    spdlog::info("[Analysis] ffmpeg-analysis exit_code={}", analyzer_rc);

    bool vbs4_ok = analyzer_rc == 0 && vr::win_utf8::file_exists_utf8(vbs4_out);
    if (vbs4_ok && !check_current_hash_budget(
            data_dir, hash, budget, "VBS4 generation", staging_dir)) {
        vr::win_utf8::delete_file_utf8(vbs4_out);
        vbs4_ok = false;
    }
    spdlog::info("[Analysis] ffmpeg-analysis vbs4_out={} exists={}", vbs4_out, vbs4_ok);
    return vbs4_ok;
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

    const VbiCodec source_codec = detect_analysis_codec(video_path);
    if (source_codec != static_cast<VbiCodec>(base.header().codec)) {
        spdlog::warn("[Analysis] generate_vac2_overlay_chunk: detected codec {} differs from base codec {}",
                     static_cast<int>(source_codec), base.header().codec);
    }

    const std::string vbs4_tmp = staging.path() + "\\" + std::string(hash) + ".overlay.tmp.vbs4";
    bool vbs4_generated = false;
    if (source_codec == VbiCodec::VVC) {
        vbs4_generated = generate_vvc_vbs4(
            exe_dir, data_dir, staging.path(), logs_dir, video_path, hash, vbs4_tmp, budget);
    } else if (ffmpeg_analysis_codec_arg(source_codec)) {
        vbs4_generated = generate_ffmpeg_vbs4(
            exe_dir, data_dir, staging.path(), logs_dir, video_path, hash,
            source_codec, vbs4_tmp, budget);
    }
    if (!vbs4_generated || !vr::win_utf8::file_exists_utf8(vbs4_tmp)) {
        spdlog::error("[Analysis] generate_vac2_overlay_chunk: deep analyzer failed");
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "failed to generate deep overlay analysis");
        return 0;
    }

    vr::analysis::Vbs4File vbs4;
    if (!vbs4.open(vbs4_tmp)) {
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        set_analysis_error(NAKI_ANALYSIS_ERR_OPEN_FAILED,
                           "failed to open generated overlay analysis");
        return 0;
    }

    vr::analysis::VachunkData chunk_data;
    if (!vr::analysis::build_overlay_vachunk_from_vbs4(
            vbs4,
            static_cast<uint32_t>(start_frame),
            static_cast<uint32_t>(end_frame),
            chunk_data)) {
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "failed to build overlay VACHUNK");
        return 0;
    }
    vbs4.close();
    vr::win_utf8::delete_file_utf8(vbs4_tmp);

    vr::analysis::VachunkKey key;
    key.kind = VachunkKind::Overlay;
    key.codec = static_cast<VbiCodec>(base.header().codec);
    key.feature_flags = chunk_data.feature_flags;
    key.base_content_revision = base.header().content_revision;
    key.generator_revision = 1;
    key.start_frame = static_cast<uint32_t>(start_frame);
    key.end_frame = static_cast<uint32_t>(end_frame);

    const uint64_t max_output_bytes =
        max_cache_bytes > 0 ? static_cast<uint64_t>(max_cache_bytes) : 0;
    if (!store.write_chunk_atomic(key, std::move(chunk_data), max_output_bytes)) {
        set_analysis_error(NAKI_ANALYSIS_ERR_INTERNAL,
                           "failed to publish overlay VACHUNK");
        return 0;
    }

    set_analysis_ok();
    spdlog::info("[Analysis] generate_vac2_overlay_chunk succeeded: hash={}, frames={}-{}",
                 hash, start_frame, end_frame);
    return 1;
}
