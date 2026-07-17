#include "analysis/analysis_ffi_abi.h"

#include "analysis/analysis_generation_service.h"
#include "analysis/analysis_manager.h"
#include "analysis/analysis_session.h"
#include "analysis/cache/overlay_chunk.h"
#include "analysis/cache/vacache_store.h"
#include "analysis/generators/analysis_generator.h"
#include "analysis/parsers/vachunk_parser.h"
#include "common/win_utf8.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct AnalysisFfiError {
    int32_t status = NAKI_ANALYSIS_OK;
    std::string message;
};

thread_local AnalysisFfiError g_last_error;
std::mutex g_generation_mutex;
std::mutex g_handle_registry_mutex;
std::unordered_map<uintptr_t, std::shared_ptr<vr::analysis::AnalysisSession>> g_handles;
std::atomic<uintptr_t> g_next_handle_id{1};
std::atomic<uint64_t> g_overlay_staging_counter{1};

constexpr uint64_t kOverlayVachunkFeatureFlags =
    VACHUNK_FEATURE_CU_GEOMETRY |
    VACHUNK_FEATURE_QP |
    VACHUNK_FEATURE_PRED_MODE |
    VACHUNK_FEATURE_MOTION_VECTORS |
    VACHUNK_FEATURE_REF_INDEXES |
    VACHUNK_FEATURE_BIT_COST;

void set_error(int32_t status, std::string message) {
    g_last_error.status = status;
    g_last_error.message = std::move(message);
}

void set_ok() {
    set_error(NAKI_ANALYSIS_OK, {});
}

const char* safe_cstr(const char* value) {
    return value ? value : "";
}

bool path_exists(const std::string& path) {
    return vr::win_utf8::file_exists_utf8(path);
}

std::string join_path(const std::string& base, const std::string& child) {
    return vr::win_utf8::path_to_utf8(
        vr::win_utf8::path_from_utf8(base) /
        vr::win_utf8::path_from_utf8(child));
}

const char* ffmpeg_analysis_codec_arg(AnalysisCodec codec) {
    switch (codec) {
    case AnalysisCodec::H264: return "h264";
    case AnalysisCodec::HEVC: return "hevc";
    case AnalysisCodec::VVC: return "vvc";
    default: return nullptr;
    }
}

std::string host_analyzer_platform_dir() {
#if defined(_WIN32)
    return "windows-x64";
#elif defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#else
    return "macos-x64";
#endif
}

std::string analyzer_executable_name() {
#if defined(_WIN32)
    return "void_ffmpeg_analyzer.exe";
#else
    return "void_ffmpeg_analyzer";
#endif
}

uint64_t current_process_id() {
#if defined(_WIN32)
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

std::string find_ffmpeg_analyzer() {
    const std::string env = vr::win_utf8::get_env_utf8(L"VOID_FFMPEG_ANALYZER");
    if (!env.empty() && path_exists(env)) return env;

    std::vector<std::string> roots;
    const std::string module_dir = vr::win_utf8::module_directory_utf8();
    if (!module_dir.empty()) roots.push_back(module_dir);

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        roots.push_back(vr::win_utf8::path_to_utf8(cwd));
        roots.push_back(vr::win_utf8::path_to_utf8(cwd / ".."));
    }

    const std::string platform = host_analyzer_platform_dir();
    const std::string executable = analyzer_executable_name();
    for (const auto& initial_root : roots) {
        auto root_path = vr::win_utf8::path_from_utf8(initial_root);
        for (int depth = 0; depth < 8 && !root_path.empty(); ++depth) {
            const std::string root = vr::win_utf8::path_to_utf8(root_path);
            const std::vector<std::string> candidates = {
                join_path(join_path(root, "../Helpers"),
                          "ffmpeg-analysis/" + executable),
                join_path(join_path(root, "tools"),
                          "ffmpeg-analysis/" + executable),
                join_path(root, executable),
                join_path(join_path(root, "native/analysis/vendor/ffmpeg/bin"),
                          platform + "/" + executable),
                join_path(join_path(root, "analysis/vendor/ffmpeg/bin"),
                          platform + "/" + executable),
            };
            for (const auto& candidate : candidates) {
                if (path_exists(candidate)) return candidate;
            }
            if (!root_path.has_parent_path() ||
                root_path.parent_path() == root_path) {
                break;
            }
            root_path = root_path.parent_path();
        }
    }
    return {};
}

int run_process(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
#if defined(_WIN32)
    auto quote_arg = [](const std::wstring& arg) {
        if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
            return arg;
        }
        std::wstring quoted(1, L'\"');
        size_t backslashes = 0;
        for (const wchar_t ch : arg) {
            if (ch == L'\\') {
                ++backslashes;
                continue;
            }
            if (ch == L'\"') {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'\"');
        return quoted;
    };

    std::wstring command_line;
    for (const auto& arg : args) {
        const std::wstring wide = vr::win_utf8::utf16_from_utf8(arg);
        if (wide.empty() && !arg.empty()) return -1;
        if (!command_line.empty()) command_line.push_back(L' ');
        command_line.append(quote_arg(wide));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
#else
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(args[0].c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

bool vachunk_matches_key(const vr::analysis::VachunkFile& chunk,
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

NakiAnalysisHandle encode_handle(uintptr_t id) {
    return reinterpret_cast<NakiAnalysisHandle>(id);
}

uintptr_t decode_handle(NakiAnalysisHandle handle) {
    return reinterpret_cast<uintptr_t>(handle);
}

std::shared_ptr<vr::analysis::AnalysisSession> pin_handle(NakiAnalysisHandle handle) {
    const uintptr_t id = decode_handle(handle);
    if (id == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_handle_registry_mutex);
    const auto it = g_handles.find(id);
    return it == g_handles.end() ? nullptr : it->second;
}

NakiAnalysisHandle register_handle(std::shared_ptr<vr::analysis::AnalysisSession> session) {
    if (!session) return nullptr;
    std::lock_guard<std::mutex> lock(g_handle_registry_mutex);
    uintptr_t id = g_next_handle_id.fetch_add(1, std::memory_order_relaxed);
    while (id == 0 || g_handles.find(id) != g_handles.end()) {
        id = g_next_handle_id.fetch_add(1, std::memory_order_relaxed);
    }
    g_handles.emplace(id, std::move(session));
    return encode_handle(id);
}

std::shared_ptr<vr::analysis::AnalysisSession> unregister_handle(NakiAnalysisHandle handle) {
    const uintptr_t id = decode_handle(handle);
    if (id == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_handle_registry_mutex);
    const auto it = g_handles.find(id);
    if (it == g_handles.end()) return nullptr;
    auto session = std::move(it->second);
    g_handles.erase(it);
    return session;
}

void fill_summary(const vr::analysis::AnalysisSession& session,
                  NakiAnalysisSummary& out,
                  int64_t pts_us = INT64_MIN,
                  int64_t dts_us = INT64_MIN) {
    const auto& base = session.vac2_base();
    const auto& header = base.header();
    std::memset(&out, 0, sizeof(out));
    out.loaded = 1;
    out.frame_count = static_cast<int32_t>(base.frames().size());
    out.packet_count = static_cast<int32_t>(base.packets().size());
    out.nalu_count = static_cast<int32_t>(base.units().size());
    out.video_width = static_cast<int32_t>(header.width);
    out.video_height = static_cast<int32_t>(header.height);
    out.time_base_num = header.time_base_num;
    out.time_base_den = header.time_base_den;
    out.current_frame_idx =
        pts_us != INT64_MIN || dts_us != INT64_MIN ? session.current_frame_idx(pts_us) : -1;
    out.codec = static_cast<int32_t>(header.codec);
}

int32_t frame_index_for_timestamp(const vr::analysis::AnalysisSession& session,
                                  int64_t pts_us,
                                  int64_t dts_us) {
    const int pts_index = pts_us != INT64_MIN ? session.current_frame_idx(pts_us) : -1;
    if (pts_index >= 0) return pts_index;
    return dts_us != INT64_MIN ? session.current_frame_idx(dts_us) : -1;
}

void fill_frame_info(const vr::analysis::AnalysisSession& session,
                     size_t index,
                     NakiFrameInfo& out) {
    const auto& base = session.vac2_base();
    const auto& frames = base.frames();
    const auto& summaries = base.frame_summaries();
    std::memset(&out, 0, sizeof(out));
    if (index >= frames.size()) return;

    const auto& frame = frames[index];
    out.poc = frame.poc;
    out.pts = frame.pts;
    out.dts = frame.dts;
    out.packet_size = static_cast<int32_t>(std::min<uint32_t>(
        frame.frame_size,
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
    out.keyframe = (frame.flags & VAC2_FRAME_FLAG_KEYFRAME) ? 1 : 0;

    for (int i = 0; i < 15; ++i) {
        out.ref_pocs_l0[i] = -1;
        out.ref_pocs_l1[i] = -1;
    }

    if (index < summaries.size()) {
        const auto& summary = summaries[index];
        out.temporal_id = summary.temporal_id;
        out.slice_type = summary.slice_type;
        out.nal_type = summary.nal_type;
        out.avg_qp = summary.qp_avg;
        out.num_ref_l0 = std::min<int32_t>(summary.num_ref_l0, 15);
        out.num_ref_l1 = std::min<int32_t>(summary.num_ref_l1, 15);
        for (int i = 0; i < out.num_ref_l0; ++i) out.ref_pocs_l0[i] = summary.ref_pocs_l0[i];
        for (int i = 0; i < out.num_ref_l1; ++i) out.ref_pocs_l1[i] = summary.ref_pocs_l1[i];
    }
}

void fill_nalu_info(const vr::analysis::AnalysisSession& session,
                    size_t index,
                    NakiNaluInfo& out) {
    const auto& units = session.vac2_base().units();
    std::memset(&out, 0, sizeof(out));
    if (index >= units.size()) return;
    const auto& unit = units[index];
    out.offset = unit.offset;
    out.size = unit.size;
    out.nal_type = unit.nal_type;
    out.temporal_id = unit.temporal_id;
    out.layer_id = unit.layer_id;
    out.flags = 0;
    if (unit.flags & VAC2_UNIT_FLAG_IS_VCL) out.flags |= 0x01;
    if (unit.flags & VAC2_UNIT_FLAG_IS_SLICE) out.flags |= 0x02;
    if (unit.flags & VAC2_UNIT_FLAG_IS_KEYFRAME) out.flags |= 0x04;
}

} // namespace

void naki_analysis_register_pts_callback(NakiAnalysisPtsCallback) {}
void naki_analysis_register_pts_callback_for_owner(const void*, NakiAnalysisPtsCallback) {}
void naki_analysis_clear_pts_callback_for_owner(const void*) {}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_abi_version() {
    return NAKI_ANALYSIS_ABI_VERSION;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_last_error(char* buf, int32_t cap) {
    if (buf && cap > 0) {
        const size_t writable = static_cast<size_t>(cap - 1);
        const size_t to_copy = std::min(writable, g_last_error.message.size());
        std::memcpy(buf, g_last_error.message.data(), to_copy);
        buf[to_copy] = '\0';
    }
    return g_last_error.status;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_summary() {
    return static_cast<int32_t>(sizeof(NakiAnalysisSummary));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_frame_info() {
    return static_cast<int32_t>(sizeof(NakiFrameInfo));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_nalu_info() {
    return static_cast<int32_t>(sizeof(NakiNaluInfo));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_frame_bucket() {
    return static_cast<int32_t>(sizeof(NakiFrameBucket));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_overlay_state() {
    return static_cast<int32_t>(sizeof(NakiOverlayState));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_summary_v2() {
    return static_cast<int32_t>(sizeof(NakiAnalysisSummaryV2));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_frame_info_v2() {
    return static_cast<int32_t>(sizeof(NakiFrameInfoV2));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_nalu_info_v2() {
    return static_cast<int32_t>(sizeof(NakiNaluInfoV2));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_frame_bucket_v2() {
    return static_cast<int32_t>(sizeof(NakiFrameBucketV2));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_sizeof_overlay_state_v2() {
    return static_cast<int32_t>(sizeof(NakiOverlayStateV2));
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT void naki_analysis_set_overlay(const NakiOverlayState* state) {
    if (!state) return;
    auto& overlay = vr::analysis::AnalysisManager::instance().overlay;
    overlay.show_cu_grid.store(state->show_cu_grid != 0, std::memory_order_release);
    overlay.show_pred_mode.store(state->show_pred_mode != 0, std::memory_order_release);
    overlay.show_qp_heatmap.store(state->show_qp_heatmap != 0, std::memory_order_release);
    overlay.show_pred_lines.store(state->show_pred_lines != 0, std::memory_order_release);
    overlay.show_cu_bit_cost_heatmap.store(state->show_cu_bit_cost_heatmap != 0,
                                           std::memory_order_release);
    overlay.opacity_permille.store(std::clamp(state->opacity_permille, 0, 1000),
                                   std::memory_order_release);
    overlay.mode.store(std::max(0, state->mode), std::memory_order_release);
    overlay.track_file_id.store(state->track_file_id, std::memory_order_release);
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_set_overlay_track(
    int32_t track_file_id,
    const char* analysis_path) {
    if (track_file_id < 0 || !analysis_path || analysis_path[0] == '\0') {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                  "track_file_id and analysis_path are required");
        return 0;
    }
    if (!vr::analysis::AnalysisManager::instance().set_overlay_track(
            track_file_id, analysis_path)) {
        set_error(NAKI_ANALYSIS_ERR_OPEN_FAILED, "failed to set overlay track");
        return 0;
    }
    set_ok();
    return 1;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT void naki_analysis_clear_overlay_tracks() {
    vr::analysis::AnalysisManager::instance().clear_overlay_tracks();
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT NakiAnalysisHandle naki_analysis_open(
    const char* analysis_path) {
    if (!analysis_path || analysis_path[0] == '\0') {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "analysis_path is required");
        return nullptr;
    }
    auto session = std::shared_ptr<vr::analysis::AnalysisSession>(
        new (std::nothrow) vr::analysis::AnalysisSession());
    if (!session) {
        set_error(NAKI_ANALYSIS_ERR_INTERNAL, "failed to allocate analysis session");
        return nullptr;
    }
    if (!session->open(analysis_path)) {
        set_error(NAKI_ANALYSIS_ERR_OPEN_FAILED, "failed to load analysis container");
        return nullptr;
    }
    auto handle = register_handle(std::move(session));
    if (!handle) {
        set_error(NAKI_ANALYSIS_ERR_INTERNAL, "failed to register analysis handle");
        return nullptr;
    }
    set_ok();
    return handle;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT void naki_analysis_close(NakiAnalysisHandle handle) {
    if (!unregister_handle(handle)) {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "analysis handle is invalid or closed");
        return;
    }
    set_ok();
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT const NakiAnalysisSummary*
naki_analysis_handle_get_summary(NakiAnalysisHandle handle) {
    thread_local NakiAnalysisSummary summary{};
    const auto session = pin_handle(handle);
    if (!session) {
        std::memset(&summary, 0, sizeof(summary));
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "analysis handle is invalid or closed");
        return &summary;
    }
    fill_summary(*session, summary);
    set_ok();
    return &summary;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_frame_index_for_timestamp(
    NakiAnalysisHandle handle,
    int64_t pts_us,
    int64_t dts_us) {
    const auto session = pin_handle(handle);
    if (!session) {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "analysis handle is invalid or closed");
        return -1;
    }
    set_ok();
    return frame_index_for_timestamp(*session, pts_us, dts_us);
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_frame_index_for_source_packet(
    NakiAnalysisHandle handle,
    int64_t packet_pos,
    int32_t packet_size,
    int32_t packet_index,
    int64_t packet_pts,
    int64_t packet_dts) {
    const auto session = pin_handle(handle);
    if (!session) {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "analysis handle is invalid or closed");
        return -1;
    }
    set_ok();
    return session->frame_idx_for_source_packet(
        packet_pos,
        packet_size,
        packet_index,
        packet_pts,
        packet_dts);
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_frames(
    NakiAnalysisHandle handle,
    NakiFrameInfo* out,
    int32_t max_count) {
    return naki_analysis_handle_get_frames_range(handle, 0, out, max_count);
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_frames_range(
    NakiAnalysisHandle handle,
    int32_t start,
    NakiFrameInfo* out,
    int32_t max_count) {
    const auto session = pin_handle(handle);
    if (!session || !out || start < 0 || max_count <= 0) {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "invalid frame range arguments");
        return 0;
    }
    const auto& frames = session->vac2_base().frames();
    int32_t produced = 0;
    for (int32_t i = 0; i < max_count && static_cast<size_t>(start + i) < frames.size(); ++i) {
        fill_frame_info(*session, static_cast<size_t>(start + i), out[i]);
        ++produced;
    }
    set_ok();
    return produced;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_nalus(
    NakiAnalysisHandle handle,
    NakiNaluInfo* out,
    int32_t max_count) {
    return naki_analysis_handle_get_nalus_range(handle, 0, out, max_count);
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_nalus_range(
    NakiAnalysisHandle handle,
    int32_t start,
    NakiNaluInfo* out,
    int32_t max_count) {
    const auto session = pin_handle(handle);
    if (!session || !out || start < 0 || max_count <= 0) {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "invalid NALU range arguments");
        return 0;
    }
    const auto& units = session->vac2_base().units();
    int32_t produced = 0;
    for (int32_t i = 0; i < max_count && static_cast<size_t>(start + i) < units.size(); ++i) {
        fill_nalu_info(*session, static_cast<size_t>(start + i), out[i]);
        ++produced;
    }
    set_ok();
    return produced;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_frame_to_nalu(
    NakiAnalysisHandle handle,
    int32_t frame_index) {
    const auto session = pin_handle(handle);
    if (!session || frame_index < 0) return -1;
    const auto& frames = session->vac2_base().frames();
    if (static_cast<size_t>(frame_index) >= frames.size()) return -1;
    set_ok();
    return static_cast<int32_t>(frames[frame_index].first_unit);
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_nalu_to_frame(
    NakiAnalysisHandle handle,
    int32_t nalu_index) {
    const auto session = pin_handle(handle);
    if (!session || nalu_index < 0) return -1;
    const auto& frames = session->vac2_base().frames();
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        const uint32_t first = frame.first_unit;
        const uint32_t end = first + frame.unit_count;
        if (static_cast<uint32_t>(nalu_index) >= first &&
            static_cast<uint32_t>(nalu_index) < end) {
            set_ok();
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_handle_get_frame_buckets(
    NakiAnalysisHandle handle,
    int32_t start,
    int32_t bucket_size,
    NakiFrameBucket* out,
    int32_t max_count) {
    const auto session = pin_handle(handle);
    if (!session || !out || start < 0 || bucket_size <= 0 || max_count <= 0) {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT, "invalid frame bucket arguments");
        return 0;
    }
    const auto& frames = session->vac2_base().frames();
    int32_t produced = 0;
    for (int32_t bucket_start = start;
         produced < max_count && static_cast<size_t>(bucket_start) < frames.size();
         bucket_start += bucket_size) {
        NakiFrameBucket& bucket = out[produced];
        std::memset(&bucket, 0, sizeof(bucket));
        bucket.start_frame = bucket_start;
        bucket.packet_size_min = INT32_MAX;
        bucket.qp_min = INT32_MAX;
        const int32_t end = std::min<int32_t>(
            bucket_start + bucket_size, static_cast<int32_t>(frames.size()));
        for (int32_t i = bucket_start; i < end; ++i) {
            NakiFrameInfo info{};
            fill_frame_info(*session, static_cast<size_t>(i), info);
            bucket.frame_count++;
            bucket.packet_size_min = std::min(bucket.packet_size_min, info.packet_size);
            bucket.packet_size_max = std::max(bucket.packet_size_max, info.packet_size);
            bucket.packet_size_sum += info.packet_size;
            bucket.qp_min = std::min(bucket.qp_min, info.avg_qp);
            bucket.qp_max = std::max(bucket.qp_max, info.avg_qp);
            bucket.qp_sum += info.avg_qp;
            bucket.keyframe_count += info.keyframe != 0 ? 1 : 0;
        }
        if (bucket.frame_count == 0) {
            bucket.packet_size_min = 0;
            bucket.qp_min = 0;
        }
        ++produced;
    }
    set_ok();
    return produced;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_generate_vac2_base(
    const char* video_path,
    const char* hash,
    const char* cache_root,
    int64_t max_cache_bytes) {
    std::lock_guard<std::mutex> lock(g_generation_mutex);
    if (!video_path || video_path[0] == '\0' || !hash || hash[0] == '\0' ||
        !cache_root || cache_root[0] == '\0') {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                  "video_path, hash, and cache_root are required");
        return 0;
    }

    vr::analysis::VacacheStore store(cache_root, hash);
    if (!store.ensure_layout()) {
        set_error(NAKI_ANALYSIS_ERR_OPEN_FAILED, "failed to create VAC2 cache layout");
        return 0;
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ticks =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    std::ostringstream tmp_name;
    tmp_name << "base." << current_process_id() << "." << ticks << ".vac.tmp";
    const std::string tmp_path = vr::win_utf8::path_to_utf8(
        vr::win_utf8::path_from_utf8(store.tmp_dir()) /
        vr::win_utf8::path_from_utf8(tmp_name.str()));
    const uint64_t max_output_bytes =
        max_cache_bytes > 0 ? static_cast<uint64_t>(max_cache_bytes) : 0;

    if (!vr::analysis::AnalysisGenerator::generate_vac2_base(
            safe_cstr(video_path), tmp_path, max_output_bytes)) {
        vr::win_utf8::delete_file_utf8(tmp_path);
        set_error(NAKI_ANALYSIS_ERR_INTERNAL, "failed to generate VAC2 base cache");
        return 0;
    }

    vr::win_utf8::delete_file_utf8(store.base_path());
    std::error_code rename_ec;
    std::filesystem::rename(
        vr::win_utf8::path_from_utf8(tmp_path),
        vr::win_utf8::path_from_utf8(store.base_path()),
        rename_ec);
    if (rename_ec) {
        vr::win_utf8::delete_file_utf8(tmp_path);
        set_error(NAKI_ANALYSIS_ERR_INTERNAL, "failed to publish VAC2 base cache");
        return 0;
    }

    vr::analysis::Vac2BaseFile verify;
    if (!store.open_base(verify)) {
        set_error(NAKI_ANALYSIS_ERR_OPEN_FAILED, "published VAC2 cache failed to reopen");
        return 0;
    }
    set_ok();
    return 1;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_generate_vac2_overlay_chunk(
    const char* video_path,
    const char* hash,
    const char* cache_root,
    int32_t start_frame,
    int32_t end_frame,
    int64_t max_cache_bytes) {
    if (!video_path || video_path[0] == '\0' || !hash || hash[0] == '\0' ||
        !cache_root || cache_root[0] == '\0' || start_frame < 0 ||
        end_frame < start_frame) {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                  "video_path, hash, cache_root, and a valid frame range are required");
        return 0;
    }

    vr::analysis::VacacheStore store(cache_root, hash);
    if (!store.ensure_layout()) {
        set_error(NAKI_ANALYSIS_ERR_OPEN_FAILED, "failed to create VAC2 cache layout");
        return 0;
    }

    vr::analysis::Vac2BaseFile base;
    if (!store.open_base(base)) {
        set_error(NAKI_ANALYSIS_ERR_OPEN_FAILED, "VAC2 base cache is missing or invalid");
        return 0;
    }
    if (static_cast<size_t>(end_frame) >= base.frames().size()) {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                  "overlay frame range exceeds VAC2 base frame count");
        return 0;
    }

    const AnalysisCodec codec = analysis_codec_from_u16(base.header().codec);
    const char* codec_arg = ffmpeg_analysis_codec_arg(codec);
    if (!codec_arg) {
        set_error(NAKI_ANALYSIS_ERR_UNSUPPORTED,
                  "overlay VACHUNK generation only supports H.264/HEVC/VVC");
        return 0;
    }

    const std::string analyzer = find_ffmpeg_analyzer();
    if (analyzer.empty()) {
        set_error(NAKI_ANALYSIS_ERR_OPEN_FAILED,
                  "void_ffmpeg_analyzer not found; set VOID_FFMPEG_ANALYZER");
        return 0;
    }

    vr::analysis::VachunkKey key;
    key.kind = VachunkKind::Overlay;
    key.codec = codec;
    key.feature_flags = kOverlayVachunkFeatureFlags;
    key.base_content_revision = base.header().content_revision;
    key.generator_revision = 3;
    key.start_frame = static_cast<uint32_t>(start_frame);
    key.end_frame = static_cast<uint32_t>(end_frame);

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ticks =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const uint64_t staging_counter =
        g_overlay_staging_counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream staging_name;
    staging_name << "overlay.ffi." << current_process_id()
                 << "." << ticks << "." << staging_counter;
    const auto staging_dir =
        vr::win_utf8::path_from_utf8(store.tmp_dir()) / staging_name.str();
    std::error_code ec;
    std::filesystem::create_directories(staging_dir, ec);
    if (ec) {
        set_error(NAKI_ANALYSIS_ERR_INTERNAL,
                  "failed to create overlay VACHUNK staging directory");
        return 0;
    }
    const std::string tmp_path = vr::win_utf8::path_to_utf8(
        staging_dir / (std::string(hash) + ".overlay.tmp.vck"));

    const int rc = run_process({
        analyzer,
        "--codec", codec_arg,
        "--input", safe_cstr(video_path),
        "--vachunk", tmp_path,
        "--start-frame", std::to_string(start_frame),
        "--end-frame", std::to_string(end_frame),
        "--base-revision", std::to_string(key.base_content_revision),
        "--generator-revision", std::to_string(key.generator_revision),
    });
    if (rc != 0 || !path_exists(tmp_path)) {
        std::filesystem::remove_all(staging_dir, ec);
        set_error(NAKI_ANALYSIS_ERR_INTERNAL,
                  "FFmpeg analyzer failed while generating overlay VACHUNK");
        return 0;
    }

    vr::analysis::VachunkFile verify;
    if (!verify.open(tmp_path) || !vachunk_matches_key(verify, key)) {
        std::filesystem::remove_all(staging_dir, ec);
        set_error(NAKI_ANALYSIS_ERR_INTERNAL,
                  "generated overlay VACHUNK does not match requested key");
        return 0;
    }
    verify.close();

    vr::analysis::VachunkData data;
    if (!vr::analysis::read_vachunk_file_data(tmp_path, data)) {
        std::filesystem::remove_all(staging_dir, ec);
        set_error(NAKI_ANALYSIS_ERR_INTERNAL,
                  "failed to read generated overlay VACHUNK");
        return 0;
    }
    const uint64_t max_output_bytes =
        max_cache_bytes > 0 ? static_cast<uint64_t>(max_cache_bytes) : 0;
    if (!store.write_chunk_atomic(key, std::move(data), max_output_bytes)) {
        std::filesystem::remove_all(staging_dir, ec);
        set_error(NAKI_ANALYSIS_ERR_INTERNAL,
                  "failed to publish generated overlay VACHUNK");
        return 0;
    }
    std::filesystem::remove_all(staging_dir, ec);
    set_ok();
    return 1;
}

namespace {

vr::analysis::AnalysisGenerationService& generation_service() {
    static vr::analysis::AnalysisGenerationService service(
        [](const vr::analysis::AnalysisGenerationOverlayChunkRequest& request,
           std::string& message) -> int32_t {
            const int32_t ok = naki_analysis_generate_vac2_overlay_chunk(
                request.video_path.c_str(),
                request.hash.c_str(),
                request.cache_root.c_str(),
                request.start_frame,
                request.end_frame,
                request.max_cache_bytes);
            if (ok != 0) return NAKI_ANALYSIS_OK;
            char buffer[256] = {};
            const int32_t status = naki_analysis_last_error(buffer, sizeof(buffer));
            message = buffer;
            return status == NAKI_ANALYSIS_OK ? NAKI_ANALYSIS_ERR_INTERNAL : status;
        });
    return service;
}

} // namespace

extern "C" NAKI_ANALYSIS_FFI_EXPORT uint64_t naki_analysis_submit_vac2_overlay_chunk(
    const char* video_path,
    const char* hash,
    const char* cache_root,
    int32_t start_frame,
    int32_t end_frame,
    int64_t max_cache_bytes,
    int32_t priority) {
    if (!video_path || video_path[0] == '\0' || !hash || hash[0] == '\0' ||
        !cache_root || cache_root[0] == '\0' || start_frame < 0 ||
        end_frame < start_frame) {
        set_error(NAKI_ANALYSIS_ERR_INVALID_ARGUMENT,
                  "video_path, hash, cache_root, and a valid frame range are required");
        return 0;
    }
    vr::analysis::AnalysisGenerationOverlayChunkRequest request;
    request.video_path = video_path;
    request.hash = hash;
    request.cache_root = cache_root;
    request.start_frame = start_frame;
    request.end_frame = end_frame;
    request.max_cache_bytes = max_cache_bytes;
    request.priority = priority;
    const uint64_t job_id = generation_service().submit_overlay_chunk(std::move(request));
    if (job_id == 0) {
        set_error(NAKI_ANALYSIS_ERR_INTERNAL, "failed to submit overlay generation job");
        return 0;
    }
    set_ok();
    return job_id;
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT int32_t naki_analysis_poll_generation_jobs(
    NakiAnalysisGenerationJobResult* out,
    int32_t max_count) {
    return generation_service().poll_results(out, max_count);
}

extern "C" NAKI_ANALYSIS_FFI_EXPORT void naki_analysis_get_generation_service_stats(
    NakiAnalysisGenerationServiceStats* out) {
    if (!out) return;
    generation_service().copy_stats(*out);
}
