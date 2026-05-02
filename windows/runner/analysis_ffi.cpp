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
#include <fstream>
#include <limits>
#include <new>
#include <atomic>
#include <mutex>
#include <memory>
#include <string>
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
    const auto& vbs3 = mgr.vbs3();
    const auto& vbi = mgr.vbi();
    const auto& vbt = mgr.vbt();

    const int vbt_packet_count = vbt.packet_count();
    s.frame_count = effective_frame_count(mgr);
    s.packet_count = vbt_packet_count;
    s.nalu_count = vbi.nalu_count();
    s.video_width = vbs3.header().width;
    s.video_height = vbs3.header().height;
    s.time_base_num = vbt.header().time_base_num;
    s.time_base_den = vbt.header().time_base_den;
    s.codec = static_cast<int32_t>(vbi.codec());

    if (g_get_current_pts_us) {
        int64_t pts_us = g_get_current_pts_us();
        s.current_frame_idx = mgr.current_frame_idx(pts_us);
    }
}

int effective_frame_count(vr::analysis::AnalysisManager& mgr) {
    const int vbs3_count = mgr.vbs3().frame_count();
    const int vbt_count = mgr.vbt().packet_count();
    if (vbs3_count <= 0 || vbt_count <= 0) return 0;
    return std::min(vbs3_count, vbt_count);
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

    auto fh = mgr.vbs3().read_frame_summary(source_index);
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
static int run_command(const std::string& cmd, const std::string& log_path = {}) {
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

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);
    return static_cast<int>(exit_code);
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
    explicit FileRawVvcSink(const std::string& path)
        : path_(path),
          out_(vr::win_utf8::path_from_utf8(path), std::ios::binary) {}

    bool is_open() const { return out_.is_open(); }

    bool write(const uint8_t* data, size_t size) override {
        out_.write(reinterpret_cast<const char*>(data),
                   static_cast<std::streamsize>(size));
        return out_.good();
    }

    bool finish() override {
        out_.close();
        return !out_.fail();
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
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

// Extract raw VVC Annex B bitstream from a video file using FFmpeg C API.
// Uses vvc_mp4toannexb BSF for proper container→Annex B conversion
// (handles extradata with parameter sets, AUD insertion, etc.).
// Falls back to manual conversion if BSF unavailable.
static bool extract_raw_vvc_to_sink(const std::string& video_path, RawVvcSink& sink) {
    FfmpegOpenTimeout timeout;
    AVFormatContext* fmt_ctx = alloc_format_context_with_timeout(timeout, std::chrono::seconds(30));
    if (!fmt_ctx) {
        spdlog::error("[Analysis] extract_raw_vvc: failed to allocate format context");
        return false;
    }
    int ret = avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        timeout.deadline_ns = 0;
        spdlog::error("[Analysis] extract_raw_vvc: open failed: {:#x}", static_cast<unsigned>(ret));
        if (fmt_ctx) avformat_close_input(&fmt_ctx);
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    timeout.deadline_ns = 0;
    if (ret < 0) {
        spdlog::error("[Analysis] extract_raw_vvc: find_stream_info failed: {:#x}", static_cast<unsigned>(ret));
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
        spdlog::error("[Analysis] extract_raw_vvc: no video stream found");
        avformat_close_input(&fmt_ctx);
        return false;
    }

    // Try vvc_mp4toannexb bitstream filter (handles extradata, AUD insertion)
    AVBSFContext* bsf_ctx = nullptr;
    bool use_bsf = false;
    const AVBitStreamFilter* bsf = av_bsf_get_by_name("vvc_mp4toannexb");
    if (bsf) {
        ret = av_bsf_alloc(bsf, &bsf_ctx);
        if (ret >= 0) {
            avcodec_parameters_copy(bsf_ctx->par_in, fmt_ctx->streams[video_idx]->codecpar);
            ret = av_bsf_init(bsf_ctx);
            if (ret >= 0) {
                use_bsf = true;
                spdlog::info("[Analysis] extract_raw_vvc: using vvc_mp4toannexb BSF");
            } else {
                spdlog::warn("[Analysis] extract_raw_vvc: BSF init failed: {:#x}, falling back to manual",
                             static_cast<unsigned>(ret));
                av_bsf_free(&bsf_ctx);
            }
        }
    } else {
        spdlog::warn("[Analysis] extract_raw_vvc: vvc_mp4toannexb BSF not found, using manual conversion");
    }

    static const uint8_t kStartCode4[] = {0, 0, 0, 1};
    size_t total_written = 0;
    int pkt_count = 0;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        spdlog::error("[Analysis] extract_raw_vvc: failed to allocate packet");
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
    spdlog::info("[Analysis] extract_raw_vvc: {} packets, {} bytes written",
                 pkt_count, total_written);
    return total_written > 0 && finished;
}

static bool extract_raw_vvc(const std::string& video_path, const std::string& out_path) {
    FileRawVvcSink sink(out_path);
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
            ok = total_written > 0;
        }
    }
    return ok;
}

static int run_vtm_stdin_command(const std::string& cmd,
                                 const std::string& log_path,
                                 const std::string& video_path) {
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

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);
    return wrote ? static_cast<int>(exit_code) : -1;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_generate(const char* video_path, const char* hash) {
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

    // ---- Step 0: VBS3 via VTM DecoderApp (optional) ----
    VbiCodec source_codec = detect_analysis_codec(video_path);
    std::string decoder_path = exe_dir + "\\tools\\vtm\\DecoderApp.exe";
    bool decoder_exists = vr::win_utf8::file_exists_utf8(decoder_path);
    spdlog::info("[Analysis] decoder={} exists={}, codec={}",
                 decoder_path, decoder_exists, static_cast<int>(source_codec));

    if (decoder_exists && source_codec == VbiCodec::VVC) {
        std::string vbs3_out = data_dir + "\\" + hash + ".tmp.vbs3";
        vr::win_utf8::delete_file_utf8(vbs3_out);

        ScopedEnvVars env;
        env.set("VTM_BINARY_STATS", vbs3_out);
        env.set("VTM_BINARY_STATS_FORMAT", "VBS3");
        env.set("VOID_VTM_STDIN_WINDOW_BYTES", "67108864");
        env.set("VOID_VTM_STDIN_WINDOW_NALUS", "4096");
        env.set("VOID_VTM_STDIN_HARD_CAP_BYTES", "268435456");

        // Build VTM log path: logs/vtm_<timestamp>_<hash>.log
        std::string logs_dir = exe_dir + "\\logs";
        vr::win_utf8::create_directory_utf8(logs_dir);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char vtm_log_name[128];
        snprintf(vtm_log_name, sizeof(vtm_log_name),
                 "vtm_%04d-%02d-%02d_%02d%02d%02d_%s.log",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, hash);
        std::string vtm_log_path = logs_dir + "\\" + vtm_log_name;

        // Run DecoderApp from the installed runtime tools/vtm directory.
        std::string cmd = "\"" + decoder_path +
            "\" -b - --TraceFile=NUL --TraceRule=\"D_BLOCK_STATISTICS_CODED:poc>=0\" -o NUL";
        spdlog::info("[Analysis] vtm stdin cmd: {}", cmd);
        spdlog::info("[Analysis] vtm log: {}", vtm_log_path);
        int vtm_rc = run_vtm_stdin_command(cmd, vtm_log_path, video_path);
        spdlog::info("[Analysis] vtm stdin exit_code={}", vtm_rc);

        bool vbs3_ok = vtm_rc == 0 && vr::win_utf8::file_exists_utf8(vbs3_out);
        if (!vbs3_ok) {
            spdlog::warn("[Analysis] VTM stdin generation failed, falling back to temp VVC file");
            vr::win_utf8::delete_file_utf8(vbs3_out);

            std::string tmp_vvc = data_dir + "\\" + hash + ".tmp.vvc";
            spdlog::info("[Analysis] extracting raw VVC to {}", tmp_vvc);
            bool demux_ok = extract_raw_vvc(video_path, tmp_vvc);
            spdlog::info("[Analysis] extract_raw_vvc ok={}", demux_ok);

            if (demux_ok && vr::win_utf8::file_exists_utf8(tmp_vvc)) {
                std::string fallback_cmd = "\"" + decoder_path + "\" -b \"" + tmp_vvc +
                    "\" --TraceFile=NUL --TraceRule=\"D_BLOCK_STATISTICS_CODED:poc>=0\" -o NUL";
                spdlog::info("[Analysis] vtm fallback cmd: {}", fallback_cmd);
                int fallback_rc = run_command(fallback_cmd, vtm_log_path);
                spdlog::info("[Analysis] vtm fallback exit_code={}", fallback_rc);
                vr::win_utf8::delete_file_utf8(tmp_vvc);
                vbs3_ok = fallback_rc == 0 && vr::win_utf8::file_exists_utf8(vbs3_out);
            } else {
                spdlog::warn("[Analysis] raw VVC extraction failed, skipping VBS3 generation");
                vr::win_utf8::delete_file_utf8(tmp_vvc);
            }
        }

        spdlog::info("[Analysis] vbs3_out={} exists={}", vbs3_out, vbs3_ok);
        if (!vbs3_ok) {
            spdlog::warn("[Analysis] VBS3 generation failed, continuing without VBS3");
        } else {
            vr::win_utf8::delete_file_utf8(data_dir + "\\" + hash + ".tmp.vvc");
        }
    } else if (decoder_exists) {
        spdlog::info("[Analysis] skipping VBS3: VTM block statistics are VVC-only");
    }

    std::string vbs3_tmp = data_dir + "\\" + hash + ".tmp.vbs3";
    if (source_codec == VbiCodec::VVC && !vr::win_utf8::file_exists_utf8(vbs3_tmp)) {
        spdlog::error("[Analysis] VVC analysis requires VBS3, but no VBS3 section was generated");
        vr::win_utf8::delete_file_utf8(vbs3_tmp);
        return 0;
    }

    // ---- Step 1+2: VBI + VBT via C++ FFmpeg (single pass) ----
    std::string vbi_out = data_dir + "\\" + hash + ".tmp.vbi";
    std::string vbt_out = data_dir + "\\" + hash + ".tmp.vbt";
    std::string vac_out = data_dir + "\\" + hash + ".vac";

    if (!vr::analysis::AnalysisGenerator::generate(video_path, vbi_out, vbt_out)) {
        spdlog::error("[Analysis] C++ generator failed");
        vr::win_utf8::delete_file_utf8(vbs3_tmp);
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
        vr::win_utf8::delete_file_utf8(vbs3_tmp);
        vr::win_utf8::delete_file_utf8(vbi_out);
        vr::win_utf8::delete_file_utf8(vbt_out);
        return 0;
    }

    const std::string vbs3_section = vr::win_utf8::file_exists_utf8(vbs3_tmp) ? vbs3_tmp : "";
    if (!vr::analysis::write_analysis_container(vac_out, vbs3_section, vbi_out, vbt_out)) {
        spdlog::error("[Analysis] failed to write analysis container: {}", vac_out);
        vr::win_utf8::delete_file_utf8(vbs3_tmp);
        vr::win_utf8::delete_file_utf8(vbi_out);
        vr::win_utf8::delete_file_utf8(vbt_out);
        return 0;
    }

    vr::win_utf8::delete_file_utf8(vbs3_tmp);
    vr::win_utf8::delete_file_utf8(vbi_out);
    vr::win_utf8::delete_file_utf8(vbt_out);
    vr::win_utf8::delete_file_utf8(data_dir + "\\" + hash + ".vbs3");
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
