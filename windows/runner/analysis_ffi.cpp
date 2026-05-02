#include "analysis_ffi.h"
#include "analysis/analysis_manager.h"
#include "analysis/generators/bitstream_indexer.h"
#include "analysis/generators/analysis_generator.h"
#include "utils.h"

#include <spdlog/spdlog.h>
#include <windows.h>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <new>
#include <string>
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

// ---- dart:ffi analysis exports ----
static NakiAnalysisSummary g_analysis_summary = {};

namespace {

const char* safe_cstr(const char* value) {
    return value ? value : "";
}

struct AnalysisHandleState {
    vr::analysis::AnalysisManager manager;
    NakiAnalysisSummary summary{};
};

AnalysisHandleState* as_analysis_handle(NakiAnalysisHandle handle) {
    return static_cast<AnalysisHandleState*>(handle);
}

void fill_analysis_summary(vr::analysis::AnalysisManager& mgr, NakiAnalysisSummary& s) {
    std::memset(&s, 0, sizeof(s));
    if (!mgr.is_loaded()) return;

    s.loaded = 1;
    const auto& vbs2 = mgr.vbs2();
    const auto& vbi = mgr.vbi();
    const auto& vbt = mgr.vbt();

    const int vbs2_frame_count = vbs2.frame_count();
    const int vbt_packet_count = vbt.packet_count();
    s.frame_count = vbs2_frame_count > 0 ? vbs2_frame_count : vbt_packet_count;
    s.packet_count = vbt_packet_count;
    s.nalu_count = vbi.nalu_count();
    s.video_width = vbs2.header().width;
    s.video_height = vbs2.header().height;
    s.time_base_num = vbt.header().time_base_num;
    s.time_base_den = vbt.header().time_base_den;
    s.codec = static_cast<int32_t>(vbi.codec());

    if (g_get_current_pts_us) {
        int64_t pts_us = g_get_current_pts_us();
        s.current_frame_idx = mgr.current_frame_idx(pts_us);
    }
}

int32_t fill_analysis_frames_range(vr::analysis::AnalysisManager& mgr,
                                   int32_t start,
                                   NakiFrameInfo* out,
                                   int32_t max_count);

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

    int vbs2_count = mgr.vbs2().frame_count();
    int vbt_count = mgr.vbt().packet_count();
    int total_count = vbs2_count > 0 ? std::min(vbs2_count, vbt_count) : vbt_count;
    if (start >= total_count) return 0;
    int count = std::min(max_count, total_count - start);

    // Fallback: no VBS2 — combine VBT timing with VBI VCL metadata.
    // Slice P/B needs codec-specific slice-header parsing; without VBS2 we only
    // promote keyframes to I and treat other coded frames as forward-coded.
    if (vbs2_count == 0) {
        const auto vcl_nalus = mgr.vbi().find_vcl_nalus();
        for (int i = 0; i < count; i++) {
            const int source_index = start + i;
            const auto& pkt = mgr.vbt().entry(source_index);
            auto& f = out[i];
            std::memset(&f, 0, sizeof(f));
            f.poc = source_index;
            f.slice_type = 1;
            f.pts = pkt.pts;
            f.dts = pkt.dts;
            f.packet_size = static_cast<int32_t>(pkt.size);
            f.keyframe = (pkt.flags & 0x01) ? 1 : 0;

            if (source_index < static_cast<int>(vcl_nalus.size())) {
                const auto& nalu = mgr.vbi().entry(vcl_nalus[source_index]);
                f.temporal_id = nalu.temporal_id;
                f.nal_type = nalu.nal_type;
                f.keyframe = (nalu.flags & VBI_FLAG_IS_KEYFRAME) ? 1 : f.keyframe;
            }
            if (f.keyframe != 0) {
                f.slice_type = 2;
            }
        }
        return count;
    }

    for (int i = 0; i < count; i++) {
        const int source_index = start + i;
        auto fh = mgr.vbs2().read_frame_header(source_index);
        const auto& pkt = mgr.vbt().entry(source_index);

        auto& f = out[i];
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
    }
    return count;
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

} // namespace

extern "C" __declspec(dllexport)
int32_t naki_analysis_load(const char* vbs2_path, const char* vbi_path, const char* vbt_path) {
    static int load_count = 0;
    LogStackUsage(fmt::format("analysis_load #{}", ++load_count).c_str());
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return mgr.load(safe_cstr(vbs2_path), safe_cstr(vbi_path), safe_cstr(vbt_path)) ? 1 : 0;
}

extern "C" __declspec(dllexport)
void naki_analysis_unload() {
    static int unload_count = 0;
    LogStackUsage(fmt::format("analysis_unload #{}", ++unload_count).c_str());
    vr::analysis::AnalysisManager::instance().unload();
}

extern "C" __declspec(dllexport)
const NakiAnalysisSummary* naki_analysis_get_summary() {
    auto& s = g_analysis_summary;
    auto& mgr = vr::analysis::AnalysisManager::instance();
    fill_analysis_summary(mgr, s);
    return &s;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frames(NakiFrameInfo* out, int32_t max_count) {
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return fill_analysis_frames(mgr, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_frames_range(int32_t start, NakiFrameInfo* out, int32_t max_count) {
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return fill_analysis_frames_range(mgr, start, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_nalus(NakiNaluInfo* out, int32_t max_count) {
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return fill_analysis_nalus(mgr, out, max_count);
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_get_nalus_range(int32_t start, NakiNaluInfo* out, int32_t max_count) {
    auto& mgr = vr::analysis::AnalysisManager::instance();
    return fill_analysis_nalus_range(mgr, start, out, max_count);
}

extern "C" __declspec(dllexport)
void naki_analysis_set_overlay(const NakiOverlayState* state) {
    if (!state) return;
    auto& overlay = vr::analysis::AnalysisManager::instance().overlay;
    overlay.show_cu_grid = state->show_cu_grid != 0;
    overlay.show_pred_mode = state->show_pred_mode != 0;
    overlay.show_qp_heatmap = state->show_qp_heatmap != 0;
}

extern "C" __declspec(dllexport)
NakiAnalysisHandle naki_analysis_open(const char* vbs2_path, const char* vbi_path, const char* vbt_path) {
    auto* handle = new (std::nothrow) AnalysisHandleState();
    if (!handle) return nullptr;
    if (!handle->manager.load(safe_cstr(vbs2_path), safe_cstr(vbi_path), safe_cstr(vbt_path))) {
        delete handle;
        return nullptr;
    }
    return handle;
}

extern "C" __declspec(dllexport)
void naki_analysis_close(NakiAnalysisHandle handle) {
    delete as_analysis_handle(handle);
}

extern "C" __declspec(dllexport)
const NakiAnalysisSummary* naki_analysis_handle_get_summary(NakiAnalysisHandle handle) {
    auto* state = as_analysis_handle(handle);
    if (!state) {
        std::memset(&g_analysis_summary, 0, sizeof(g_analysis_summary));
        return &g_analysis_summary;
    }
    fill_analysis_summary(state->manager, state->summary);
    return &state->summary;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frames(NakiAnalysisHandle handle, NakiFrameInfo* out, int32_t max_count) {
    auto* state = as_analysis_handle(handle);
    return state ? fill_analysis_frames(state->manager, out, max_count) : 0;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_frames_range(NakiAnalysisHandle handle, int32_t start, NakiFrameInfo* out, int32_t max_count) {
    auto* state = as_analysis_handle(handle);
    return state ? fill_analysis_frames_range(state->manager, start, out, max_count) : 0;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_nalus(NakiAnalysisHandle handle, NakiNaluInfo* out, int32_t max_count) {
    auto* state = as_analysis_handle(handle);
    return state ? fill_analysis_nalus(state->manager, out, max_count) : 0;
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_handle_get_nalus_range(NakiAnalysisHandle handle, int32_t start, NakiNaluInfo* out, int32_t max_count) {
    auto* state = as_analysis_handle(handle);
    return state ? fill_analysis_nalus_range(state->manager, start, out, max_count) : 0;
}

// ---- Analysis generation ----

static std::string get_exe_dir() {
    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string dir(exe_path);
    auto last_sep = dir.find_last_of("\\/");
    if (last_sep != std::string::npos) dir = dir.substr(0, last_sep);
    return dir;
}

static VbiCodec detect_analysis_codec(const char* video_path) {
    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, video_path, nullptr, nullptr);
    if (ret >= 0) {
        ret = avformat_find_stream_info(fmt_ctx, nullptr);
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
    }
    return vr::analysis::BitstreamIndexer::codec_from_path(video_path);
}

// Run a command via CreateProcess. Returns exit code, or -1 on CreateProcess failure.
// If log_path is non-empty, stdout+stderr are redirected to that file.
static int run_command(const std::string& cmd, const std::string& log_path = {}) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };  // inheritable
    HANDLE hLogFile = INVALID_HANDLE_VALUE;

    if (!log_path.empty()) {
        hLogFile = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
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

    std::string cmdline = cmd;
    if (!CreateProcessA(
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

// Get a Windows env variable value as std::string.
static std::string get_env_var(const char* name) {
    char buf[32768];
    DWORD len = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (len > 0 && len < sizeof(buf)) ? std::string(buf, len) : std::string();
}

// RAII helper: temporarily set env vars, restore on destruction.
struct ScopedEnvVars {
    std::vector<std::pair<std::string, std::string>> saved;

    void set(const char* name, const std::string& value) {
        saved.emplace_back(name, get_env_var(name));
        SetEnvironmentVariableA(name, value.c_str());
    }

    ~ScopedEnvVars() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (it->second.empty()) {
                SetEnvironmentVariableA(it->first.c_str(), nullptr);
            } else {
                SetEnvironmentVariableA(it->first.c_str(), it->second.c_str());
            }
        }
    }
};

// Extract raw VVC Annex B bitstream from a video file using FFmpeg C API.
// Uses vvc_mp4toannexb BSF for proper container→Annex B conversion
// (handles extradata with parameter sets, AUD insertion, etc.).
// Falls back to manual conversion if BSF unavailable.
static bool extract_raw_vvc(const std::string& video_path, const std::string& out_path) {
    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        spdlog::error("[Analysis] extract_raw_vvc: open failed: {:#x}", static_cast<unsigned>(ret));
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
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

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        spdlog::error("[Analysis] extract_raw_vvc: cannot create {}", out_path);
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
                out.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
                total_written += static_cast<size_t>(pkt->size);
                av_packet_unref(pkt);
            }
        } else {
            const uint8_t* data = pkt->data;
            int len = pkt->size;

            if ((len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
                (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)) {
                out.write(reinterpret_cast<const char*>(data), len);
                total_written += static_cast<size_t>(len);
            } else {
                int pos = 0;
                while (pos + 4 < len) {
                    uint32_t nalu_len = (static_cast<uint32_t>(data[pos]) << 24) |
                                        (static_cast<uint32_t>(data[pos + 1]) << 16) |
                                        (static_cast<uint32_t>(data[pos + 2]) << 8) |
                                        static_cast<uint32_t>(data[pos + 3]);
                    if (nalu_len == 0 || pos + 4 + static_cast<int>(nalu_len) > len) break;
                    out.write(reinterpret_cast<const char*>(kStartCode4), 4);
                    out.write(reinterpret_cast<const char*>(data + pos + 4), nalu_len);
                    total_written += 4 + static_cast<size_t>(nalu_len);
                    pos += 4 + static_cast<int>(nalu_len);
                }
            }
            av_packet_unref(pkt);
        }
    }

    // Flush BSF
    if (use_bsf) {
        av_bsf_send_packet(bsf_ctx, nullptr);
        while (av_bsf_receive_packet(bsf_ctx, pkt) == 0) {
            out.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
            total_written += static_cast<size_t>(pkt->size);
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
    if (bsf_ctx) av_bsf_free(&bsf_ctx);
    avformat_close_input(&fmt_ctx);
    out.close();

    if (total_written == 0) {
        spdlog::info("[Analysis] extract_raw_vvc: FFmpeg produced no packets, trying raw Annex-B fallback");
        if (vr::analysis::BitstreamIndexer::write_annex_b_file(
                video_path, VbiCodec::VVC, out_path)) {
            std::ifstream fallback(out_path, std::ios::binary | std::ios::ate);
            total_written = fallback ? static_cast<size_t>(fallback.tellg()) : 0;
        }
    }

    spdlog::info("[Analysis] extract_raw_vvc: {} packets, {} bytes written", pkt_count, total_written);
    return total_written > 0 && out.good();
}

extern "C" __declspec(dllexport)
int32_t naki_analysis_generate(const char* video_path, const char* hash) {
    std::string exe_dir = get_exe_dir();
    std::string data_dir = exe_dir + "\\cache";

    spdlog::info("[Analysis] generate: video_path={}, hash={}", video_path, hash);
    spdlog::info("[Analysis] exe_dir={}", exe_dir);
    spdlog::info("[Analysis] data_dir={}", data_dir);

    // Ensure data directory exists
    CreateDirectoryA(data_dir.c_str(), nullptr);

    // ---- Step 0: VBS2 via VTM DecoderApp (optional) ----
    VbiCodec source_codec = detect_analysis_codec(video_path);
    std::string decoder_path = exe_dir + "\\tools\\vtm\\DecoderApp.exe";
    bool decoder_exists = GetFileAttributesA(decoder_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    spdlog::info("[Analysis] decoder={} exists={}, codec={}",
                 decoder_path, decoder_exists, static_cast<int>(source_codec));

    if (decoder_exists && source_codec == VbiCodec::VVC) {
        std::string vbs2_out = data_dir + "\\" + hash + ".vbs2";

        // Extract raw VVC bitstream via FFmpeg C API (no subprocess)
        std::string tmp_vvc = data_dir + "\\" + hash + ".tmp.vvc";
        spdlog::info("[Analysis] extracting raw VVC to {}", tmp_vvc);
        bool demux_ok = extract_raw_vvc(video_path, tmp_vvc);
        spdlog::info("[Analysis] extract_raw_vvc ok={}", demux_ok);

        if (demux_ok && GetFileAttributesA(tmp_vvc.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // Set VTM_BINARY_STATS env var for DecoderApp
            ScopedEnvVars env;
            env.set("VTM_BINARY_STATS", vbs2_out);

            // Build VTM log path: logs/vtm_<timestamp>_<hash>.log
            std::string logs_dir = exe_dir + "\\logs";
            CreateDirectoryA(logs_dir.c_str(), nullptr);
            SYSTEMTIME st;
            GetLocalTime(&st);
            char vtm_log_name[128];
            snprintf(vtm_log_name, sizeof(vtm_log_name),
                     "vtm_%04d-%02d-%02d_%02d%02d%02d_%s.log",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, hash);
            std::string vtm_log_path = logs_dir + "\\" + vtm_log_name;

            // Run DecoderApp from the installed runtime tools/vtm directory.
            std::string cmd = "\"" + decoder_path + "\" -b \"" + tmp_vvc +
                "\" --TraceFile=NUL --TraceRule=\"D_BLOCK_STATISTICS_CODED:poc>=0\" -o NUL";
            spdlog::info("[Analysis] vtm cmd: {}", cmd);
            spdlog::info("[Analysis] vtm log: {}", vtm_log_path);
            int vtm_rc = run_command(cmd, vtm_log_path);
            spdlog::info("[Analysis] vtm exit_code={}", vtm_rc);

            // Clean up temp .vvc
            DeleteFileA(tmp_vvc.c_str());

            bool vbs2_ok = GetFileAttributesA(vbs2_out.c_str()) != INVALID_FILE_ATTRIBUTES;
            spdlog::info("[Analysis] vbs2_out={} exists={}", vbs2_out, vbs2_ok);
            if (!vbs2_ok) {
                spdlog::warn("[Analysis] VBS2 generation failed, continuing without VBS2");
            }
        } else {
            spdlog::warn("[Analysis] raw VVC extraction failed, skipping VBS2 generation");
            DeleteFileA(tmp_vvc.c_str());
        }
    } else if (decoder_exists) {
        spdlog::info("[Analysis] skipping VBS2: VTM block statistics are VVC-only");
    }

    // ---- Step 1+2: VBI + VBT via C++ FFmpeg (single pass) ----
    std::string vbi_out = data_dir + "\\" + hash + ".vbi";
    std::string vbt_out = data_dir + "\\" + hash + ".vbt";

    if (!vr::analysis::AnalysisGenerator::generate(video_path, vbi_out, vbt_out)) {
        spdlog::error("[Analysis] C++ generator failed");
        return 0;
    }

    // Verify outputs exist
    bool vbi_ok = GetFileAttributesA(vbi_out.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool vbt_ok = GetFileAttributesA(vbt_out.c_str()) != INVALID_FILE_ATTRIBUTES;
    spdlog::info("[Analysis] vbi_out={} exists={}", vbi_out, vbi_ok);
    spdlog::info("[Analysis] vbt_out={} exists={}", vbt_out, vbt_ok);
    if (!vbi_ok || !vbt_ok) {
        spdlog::error("[Analysis] output files missing after generation");
        return 0;
    }

    spdlog::info("[Analysis] generation succeeded");
    return 1;
}
