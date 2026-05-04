#include "analysis_ffi.h"
#include "analysis/analysis_manager.h"
#include "analysis/generators/bitstream_indexer.h"
#include "analysis/generators/analysis_generator.h"
#include "analysis/parsers/analysis_container.h"
#include "common/win_utf8.h"
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
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

// Callback registered by video_renderer_plugin to provide current PTS.
// Avoids analysis_ffi needing to know about vr::Renderer.
static int64_t (*g_get_current_pts_us)() = nullptr;

void naki_analysis_register_pts_callback(int64_t (*cb)()) {
    g_get_current_pts_us = cb;
}

namespace {

std::mutex g_analysis_mutex;
std::mutex g_analysis_generate_mutex;

const char* safe_cstr(const char* value) {
    return value ? value : "";
}

struct AnalysisHandleState {
    vr::analysis::AnalysisManager manager;
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

int effective_frame_count(vr::analysis::AnalysisManager& mgr);

void fill_analysis_summary(vr::analysis::AnalysisManager& mgr, NakiAnalysisSummary& s) {
    std::memset(&s, 0, sizeof(s));
    s.current_frame_idx = -1;
    if (!mgr.is_loaded()) return;

    s.loaded = 1;
    const auto& vbi = mgr.vbi();
    const auto& vbt = mgr.vbt();

    const int vbt_packet_count = vbt.packet_count();
    s.frame_count = effective_frame_count(mgr);
    s.packet_count = vbt_packet_count;
    s.nalu_count = vbi.nalu_count();
    s.video_width = mgr.video_width();
    s.video_height = mgr.video_height();
    s.time_base_num = vbt.header().time_base_num;
    s.time_base_den = vbt.header().time_base_den;
    s.codec = static_cast<int32_t>(vbi.codec());

    if (g_get_current_pts_us) {
        int64_t pts_us = g_get_current_pts_us();
        s.current_frame_idx = mgr.current_frame_idx(pts_us);
    }
}

int effective_frame_count(vr::analysis::AnalysisManager& mgr) {
    const int vbs4_count = mgr.frame_count();
    const int vbt_count = mgr.vbt().packet_count();
    if (vbs4_count <= 0 || vbt_count <= 0) return 0;
    return std::min(vbs4_count, vbt_count);
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
    if (!mgr.is_loaded() || source_index < 0) return false;

    int total_count = effective_frame_count(mgr);
    if (source_index >= total_count) return false;

    auto fh = mgr.read_frame_summary(source_index);
    const auto& pkt = mgr.vbt().entry(source_index);

    std::memset(&f, 0, sizeof(f));
    f.poc = fh.poc;
    f.temporal_id = fh.temporal_id;
    f.slice_type = fh.slice_type;
    f.nal_type = fh.nal_unit_type;
    f.avg_qp = fh.avg_qp;
    f.num_ref_l0 = fh.num_ref_l0;
    f.num_ref_l1 = fh.num_ref_l1;
    std::memcpy(f.ref_pocs_l0, fh.ref_pocs_l0, sizeof(fh.ref_pocs_l0));
    std::memcpy(f.ref_pocs_l1, fh.ref_pocs_l1, sizeof(fh.ref_pocs_l1));
    f.pts = pkt.pts;
    f.dts = pkt.dts;
    f.packet_size = static_cast<int32_t>(pkt.size);
    f.keyframe = (pkt.flags & 0x01) ? 1 : 0;
    return true;
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

    int total_count = mgr.vbi().nalu_count();
    if (start >= total_count) return 0;
    int count = std::min(max_count, total_count - start);
    for (int i = 0; i < count; i++) {
        const auto& e = mgr.vbi().entry(start + i);
        auto& n = out[i];
        n.offset = e.offset;
        n.size = e.size;
        n.nal_type = e.nal_type;
        n.temporal_id = e.temporal_id;
        n.layer_id = e.layer_id;
        n.flags = e.flags;
    }
    return count;
}

int32_t frame_to_nalu(vr::analysis::AnalysisManager& mgr, int32_t frame_index) {
    if (!mgr.is_loaded() || frame_index < 0) return -1;
    int frame = 0;
    const auto& vbi = mgr.vbi();
    for (int i = 0; i < vbi.nalu_count(); ++i) {
        if ((vbi.entry(i).flags & VBI_FLAG_IS_VCL) == 0) continue;
        if (frame == frame_index) return i;
        ++frame;
    }
    return -1;
}

int32_t nalu_to_frame(vr::analysis::AnalysisManager& mgr, int32_t nalu_index) {
    if (!mgr.is_loaded() || nalu_index < 0 || nalu_index >= mgr.vbi().nalu_count()) {
        return -1;
    }
    int frame = 0;
    const auto& vbi = mgr.vbi();
    for (int i = 0; i <= nalu_index; ++i) {
        if ((vbi.entry(i).flags & VBI_FLAG_IS_VCL) == 0) continue;
        if (i == nalu_index) return frame;
        ++frame;
    }
    return -1;
}

int32_t fill_analysis_frame_buckets(vr::analysis::AnalysisManager& mgr,
                                    int32_t start,
                                    int32_t bucket_size,
                                    NakiFrameBucket* out,
                                    int32_t max_count) {
    if (!out || max_count <= 0 || bucket_size <= 0) return 0;
    if (!mgr.is_loaded() || start < 0) return 0;

    int total_count = effective_frame_count(mgr);
    if (start >= total_count) return 0;

    int produced = 0;
    int bucket_start = start;
    while (produced < max_count && bucket_start < total_count) {
        const int count = std::min(bucket_size, total_count - bucket_start);
        auto& bucket = out[produced];
        std::memset(&bucket, 0, sizeof(bucket));
        bucket.start_frame = bucket_start;
        bucket.frame_count = count;
        bucket.packet_size_min = std::numeric_limits<int32_t>::max();
        bucket.qp_min = std::numeric_limits<int32_t>::max();

        for (int i = 0; i < count; ++i) {
            NakiFrameInfo f{};
            if (!fill_analysis_frame_at(mgr, bucket_start + i, f)) break;
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
    static int load_count = 0;
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    LogStackUsage(fmt::format("analysis_load #{}", ++load_count).c_str());
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return mgr.load(safe_cstr(analysis_path)) ? 1 : 0;
}

extern "C" __declspec(dllexport)
void naki_analysis_unload() {
    static int unload_count = 0;
    std::lock_guard<std::mutex> lock(g_analysis_mutex);
    LogStackUsage(fmt::format("analysis_unload #{}", ++unload_count).c_str());
    vr::analysis::AnalysisManager::instance().unload();
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
}

extern "C" __declspec(dllexport)
NakiAnalysisHandle naki_analysis_open(const char* analysis_path) {
    try {
        auto state = std::shared_ptr<AnalysisHandleState>(new (std::nothrow) AnalysisHandleState());
        if (!state) return nullptr;
        if (!state->manager.load(safe_cstr(analysis_path))) {
            return nullptr;
        }
        auto handle = register_analysis_handle(state);
        if (!handle) {
            state->manager.unload();
        }
        return handle;
    } catch (const std::exception& e) {
        spdlog::error("[analysis_ffi] naki_analysis_open failed: {}", e.what());
    } catch (...) {
        spdlog::error("[analysis_ffi] naki_analysis_open failed: unknown exception");
    }
    return nullptr;
}

extern "C" __declspec(dllexport)
void naki_analysis_close(NakiAnalysisHandle handle) {
    auto state = unregister_analysis_handle(handle);
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed = true;
    state->manager.unload();
}

extern "C" __declspec(dllexport)
const NakiAnalysisSummary* naki_analysis_handle_get_summary(NakiAnalysisHandle handle) {
    thread_local NakiAnalysisSummary summary{};
    auto state = pin_analysis_handle(handle);
    if (!state) {
        std::memset(&summary, 0, sizeof(summary));
        return &summary;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
        std::memset(&summary, 0, sizeof(summary));
    } else {
        fill_analysis_summary(state->manager, summary);
    }
    return &summary;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frames(NakiAnalysisHandle handle, NakiFrameInfo* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->closed ? 0 : fill_analysis_frames(state->manager, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frames_range(NakiAnalysisHandle handle, int32_t start, NakiFrameInfo* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->closed ? 0 : fill_analysis_frames_range(state->manager, start, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_nalus(NakiAnalysisHandle handle, NakiNaluInfo* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->closed ? 0 : fill_analysis_nalus(state->manager, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_nalus_range(NakiAnalysisHandle handle, int32_t start, NakiNaluInfo* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->closed ? 0 : fill_analysis_nalus_range(state->manager, start, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_frame_to_nalu(NakiAnalysisHandle handle, int32_t frame_index) {
    auto state = pin_analysis_handle(handle);
    if (!state) return -1;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->closed ? -1 : frame_to_nalu(state->manager, frame_index);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_nalu_to_frame(NakiAnalysisHandle handle, int32_t nalu_index) {
    auto state = pin_analysis_handle(handle);
    if (!state) return -1;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->closed ? -1 : nalu_to_frame(state->manager, nalu_index);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frame_buckets(NakiAnalysisHandle handle, int32_t start, int32_t bucket_size, NakiFrameBucket* out, int32_t max_count) {
    auto state = pin_analysis_handle(handle);
    if (!state) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->closed ? 0 : fill_analysis_frame_buckets(state->manager, start, bucket_size, out, max_count);
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
    for (const auto& entry : std::filesystem::directory_iterator(
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
                                            const char* hash) {
    if (!hash || hash[0] == '\0') return 0;
    uint64_t total = 0;
    const std::vector<const char*> suffixes = {
        ".vac", ".tmp.vbs4", ".tmp.vbi", ".tmp.vbt", ".tmp.vvc",
        ".vbs4", ".vbi", ".vbt", ".vbs2",
    };
    for (const auto* suffix : suffixes) {
        total += file_size_utf8(data_dir + "\\" + hash + suffix);
    }
    return total;
}

static bool check_current_hash_budget(const std::string& data_dir,
                                      const char* hash,
                                      const NativeCacheBudget& budget,
                                      const char* stage) {
    if (!budget.limited) return true;
    const uint64_t used = current_hash_artifact_bytes(data_dir, hash);
    if (used <= budget.available_for_current_hash) return true;
    spdlog::warn("[Analysis] cache limit exceeded during {}: current_hash={} available={}",
                 stage, used, budget.available_for_current_hash);
    return false;
}

static uint64_t remaining_current_hash_budget(const std::string& data_dir,
                                              const char* hash,
                                              const NativeCacheBudget& budget) {
    if (!budget.limited) return 0;
    const uint64_t used = current_hash_artifact_bytes(data_dir, hash);
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

static VbiCodec detect_analysis_codec(const char* video_path) {
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

// Extract a raw Annex B bitstream from a container using FFmpeg C API.
// H.264/HEVC/VVC MP4 samples are length-prefixed, so use the matching
// mp4toannexb BSF when available and fall back to a conservative manual path.
static bool extract_raw_annex_b_to_sink(const std::string& video_path,
                                        const char* bsf_name,
                                        const char* log_label,
                                        RawVvcSink& sink) {
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

static std::string make_analysis_tool_log_path(const std::string& exe_dir,
                                               const char* tool_tag,
                                               const char* hash) {
    std::string logs_dir = exe_dir + "\\logs";
    vr::win_utf8::create_directory_utf8(logs_dir);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char log_name[160];
    snprintf(log_name, sizeof(log_name),
             "%s_%04d-%02d-%02d_%02d%02d%02d_%s.log",
             tool_tag,
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, hash);
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

static bool generate_vvc_vbs4(const std::string& exe_dir,
                              const std::string& data_dir,
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

    const std::string vtm_log_path = make_analysis_tool_log_path(exe_dir, "vtm", hash);
    std::string cmd = "\"" + decoder_path +
        "\" -b - --TraceFile=NUL --TraceRule=\"D_BLOCK_STATISTICS_CODED:poc>=0\" -o NUL";
    spdlog::info("[Analysis] vtm stdin cmd: {}", cmd);
    spdlog::info("[Analysis] vtm log: {}", vtm_log_path);
    const uint64_t stdin_budget = remaining_current_hash_budget(data_dir, hash, budget);
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

        const std::string tmp_vvc = data_dir + "\\" + hash + ".tmp.vvc";
        spdlog::info("[Analysis] extracting raw VVC to {}", tmp_vvc);
        const uint64_t extract_budget = remaining_current_hash_budget(data_dir, hash, budget);
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
            check_current_hash_budget(data_dir, hash, budget, "raw VVC extraction")) {
            const std::string fallback_cmd = "\"" + decoder_path + "\" -b \"" + tmp_vvc +
                "\" --TraceFile=NUL --TraceRule=\"D_BLOCK_STATISTICS_CODED:poc>=0\" -o NUL";
            spdlog::info("[Analysis] vtm fallback cmd: {}", fallback_cmd);
            const int fallback_rc = run_command(
                fallback_cmd,
                vtm_log_path,
                {tmp_vvc, vbs4_out},
                budget.limited ? budget.available_for_current_hash : 0);
            spdlog::info("[Analysis] vtm fallback exit_code={}", fallback_rc);
            vr::win_utf8::delete_file_utf8(tmp_vvc);
            vbs4_ok = fallback_rc == 0 && vr::win_utf8::file_exists_utf8(vbs4_out);
        } else {
            spdlog::warn("[Analysis] raw VVC extraction failed, skipping VBS4 generation");
            vr::win_utf8::delete_file_utf8(tmp_vvc);
        }
    }

    if (vbs4_ok && !check_current_hash_budget(data_dir, hash, budget, "VBS4 generation")) {
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
        exe_dir, "ffmpeg_analysis", hash);
    const std::string cmd = "\"" + analyzer_path +
        "\" --codec " + codec_arg + " --input \"" + video_path +
        "\" --vbs4 \"" + vbs4_out + "\"";
    spdlog::info("[Analysis] ffmpeg-analysis cmd: {}", cmd);
    spdlog::info("[Analysis] ffmpeg-analysis log: {}", analyzer_log_path);
    const uint64_t vbs4_budget = remaining_current_hash_budget(data_dir, hash, budget);
    if (budget.limited && vbs4_budget == 0) return false;
    const int analyzer_rc = run_command(
        cmd,
        analyzer_log_path,
        {vbs4_out},
        budget.limited ? vbs4_budget : 0);
    spdlog::info("[Analysis] ffmpeg-analysis exit_code={}", analyzer_rc);

    bool vbs4_ok = analyzer_rc == 0 && vr::win_utf8::file_exists_utf8(vbs4_out);
    if (vbs4_ok && !check_current_hash_budget(data_dir, hash, budget, "VBS4 generation")) {
        vr::win_utf8::delete_file_utf8(vbs4_out);
        vbs4_ok = false;
    }
    spdlog::info("[Analysis] ffmpeg-analysis vbs4_out={} exists={}", vbs4_out, vbs4_ok);
    return vbs4_ok;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_generate(const char* video_path, const char* hash, int64_t max_cache_bytes) {
    std::lock_guard<std::mutex> lock(g_analysis_generate_mutex);
    if (!video_path || video_path[0] == '\0' || !hash || hash[0] == '\0') {
        spdlog::error("[Analysis] generate: video_path and hash must be non-empty");
        return 0;
    }

    std::string exe_dir = get_exe_dir();
    std::string data_dir = exe_dir + "\\cache";

    spdlog::info("[Analysis] generate: video_path={}, hash={}", video_path, hash);
    spdlog::info("[Analysis] exe_dir={}", exe_dir);
    spdlog::info("[Analysis] data_dir={}", data_dir);

    // Ensure data directory exists
    vr::win_utf8::create_directory_utf8(data_dir);
    const NativeCacheBudget budget =
        compute_native_cache_budget(data_dir, hash, max_cache_bytes);
    if (budget.limited && budget.available_for_current_hash == 0) {
        spdlog::warn("[Analysis] cache limit leaves no room for analysis generation");
        return 0;
    }

    // ---- Step 0: VBS4 via codec-specific decoder instrumentation ----
    VbiCodec source_codec = detect_analysis_codec(video_path);
    std::string vbs4_tmp = data_dir + "\\" + hash + ".tmp.vbs4";
    bool vbs4_generated = false;
    spdlog::info("[Analysis] source codec={}", static_cast<int>(source_codec));
    if (source_codec == VbiCodec::VVC) {
        vbs4_generated = generate_vvc_vbs4(exe_dir, data_dir, video_path, hash, vbs4_tmp, budget);
    } else if (ffmpeg_analysis_codec_arg(source_codec)) {
        vbs4_generated = generate_ffmpeg_vbs4(
            exe_dir, data_dir, video_path, hash, source_codec, vbs4_tmp, budget);
    } else {
        spdlog::info("[Analysis] no VBS4 producer registered for codec={}",
                     static_cast<int>(source_codec));
    }

    if ((source_codec == VbiCodec::VVC || ffmpeg_analysis_codec_arg(source_codec)) &&
        (!vbs4_generated || !vr::win_utf8::file_exists_utf8(vbs4_tmp))) {
        spdlog::error("[Analysis] codec={} analysis requires VBS4, but no VBS4 section was generated",
                      static_cast<int>(source_codec));
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        return 0;
    }

    // ---- Step 1+2: VBI + VBT via C++ FFmpeg (single pass) ----
    std::string vbi_out = data_dir + "\\" + hash + ".tmp.vbi";
    std::string vbt_out = data_dir + "\\" + hash + ".tmp.vbt";
    std::string vac_out = data_dir + "\\" + hash + ".vac";

    const uint64_t vbi_vbt_budget =
        remaining_current_hash_budget(data_dir, hash, budget);
    if (budget.limited && vbi_vbt_budget == 0) {
        spdlog::error("[Analysis] cache limit leaves no room for VBI/VBT generation");
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        return 0;
    }
    if (!vr::analysis::AnalysisGenerator::generate(video_path, vbi_out, vbt_out, vbi_vbt_budget)) {
        spdlog::error("[Analysis] C++ generator failed");
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        vr::win_utf8::delete_file_utf8(vbi_out);
        vr::win_utf8::delete_file_utf8(vbt_out);
        return 0;
    }
    if (!check_current_hash_budget(data_dir, hash, budget, "VBI/VBT generation")) {
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        vr::win_utf8::delete_file_utf8(vbi_out);
        vr::win_utf8::delete_file_utf8(vbt_out);
        return 0;
    }

    // Verify outputs exist
    bool vbi_ok = vr::win_utf8::file_exists_utf8(vbi_out);
    bool vbt_ok = vr::win_utf8::file_exists_utf8(vbt_out);
    spdlog::info("[Analysis] vbi_out={} exists={}", vbi_out, vbi_ok);
    spdlog::info("[Analysis] vbt_out={} exists={}", vbt_out, vbt_ok);
    if (!vbi_ok || !vbt_ok) {
        spdlog::error("[Analysis] output files missing after generation");
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        vr::win_utf8::delete_file_utf8(vbi_out);
        vr::win_utf8::delete_file_utf8(vbt_out);
        return 0;
    }

    const std::string vbs4_section = vr::win_utf8::file_exists_utf8(vbs4_tmp) ? vbs4_tmp : "";
    const uint64_t vac_budget =
        remaining_current_hash_budget(data_dir, hash, budget);
    if (budget.limited && vac_budget == 0) {
        spdlog::error("[Analysis] cache limit leaves no room for VAC container");
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        vr::win_utf8::delete_file_utf8(vbi_out);
        vr::win_utf8::delete_file_utf8(vbt_out);
        return 0;
    }
    if (!vr::analysis::write_analysis_container(
            vac_out, vbs4_section, vbi_out, vbt_out, vac_budget)) {
        spdlog::error("[Analysis] failed to write analysis container: {}", vac_out);
        vr::win_utf8::delete_file_utf8(vbs4_tmp);
        vr::win_utf8::delete_file_utf8(vbi_out);
        vr::win_utf8::delete_file_utf8(vbt_out);
        return 0;
    }

    vr::win_utf8::delete_file_utf8(vbs4_tmp);
    vr::win_utf8::delete_file_utf8(vbi_out);
    vr::win_utf8::delete_file_utf8(vbt_out);
    vr::win_utf8::delete_file_utf8(data_dir + "\\" + hash + ".vbs4");
    vr::win_utf8::delete_file_utf8(data_dir + "\\" + hash + ".vbi");
    vr::win_utf8::delete_file_utf8(data_dir + "\\" + hash + ".vbt");
    vr::win_utf8::delete_file_utf8(data_dir + "\\" + hash + ".vbs2");

    if (!vr::win_utf8::file_exists_utf8(vac_out)) {
        spdlog::error("[Analysis] container missing after generation: {}", vac_out);
        return 0;
    }

    spdlog::info("[Analysis] generation succeeded");
    return 1;
}
