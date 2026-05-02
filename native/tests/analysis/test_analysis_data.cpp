#include "test_analysis_data.h"
#include "analysis/generators/analysis_generator.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#include <filesystem>
#include <fstream>
#include <mutex>
#include <cstdlib>

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

#ifndef VTM_DECODER_PATH
#define VTM_DECODER_PATH ""
#endif

static const std::string kTestVideo = std::string(VIDEO_TEST_DIR) + "/h266_10s_1920x1080.mp4";

// ============================================================================
// Win32 helpers (adapted from analysis_ffi.cpp)
// ============================================================================

#ifdef _WIN32
#include <windows.h>

static int run_command(const std::string& cmd, const std::string& log_path = {}) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
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

static std::string get_env_var(const char* name) {
    char buf[32768];
    DWORD len = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (len > 0 && len < sizeof(buf)) ? std::string(buf, len) : std::string();
}

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
#endif // _WIN32

// ============================================================================
// AnalysisTestData implementation
// ============================================================================

static std::once_flag g_once;
static bool g_result = false;

AnalysisTestData& AnalysisTestData::instance() {
    static AnalysisTestData data;
    return data;
}

static void atexit_cleanup() {
    AnalysisTestData::instance().cleanup();
}

bool AnalysisTestData::ensure() {
    std::call_once(g_once, [this]() {
        spdlog::info("[TestData] generating test data from {}", kTestVideo);

        // Check source video exists
        if (!std::filesystem::exists(kTestVideo)) {
            spdlog::error("[TestData] source video not found: {}", kTestVideo);
            return;
        }

        // Create temp directory
        temp_dir_ = (std::filesystem::temp_directory_path() / "void_player_analysis_test").string();
        std::filesystem::create_directories(temp_dir_);

        vbi_path_  = temp_dir_ + "/test.vbi";
        vbt_path_  = temp_dir_ + "/test.vbt";
        vbs3_path_ = temp_dir_ + "/test.vbs3";
        raw_vvc_path_ = temp_dir_ + "/test.vvc";

        // Step 1: VBI + VBT via C++ AnalysisGenerator
        if (!generate_vbi_vbt()) return;

        // Step 2: Raw VVC extraction via FFmpeg C API
        if (!extract_raw_vvc()) return;

        // Step 3: VBS3 via VTM DecoderApp
        if (!generate_vbs3()) return;

        ok_ = true;
        std::atexit(atexit_cleanup);
        spdlog::info("[TestData] all test data generated successfully");
    });
    return ok_;
}

void AnalysisTestData::cleanup() {
    if (cleaned_up_) return;
    cleaned_up_ = true;
    if (!temp_dir_.empty() && std::filesystem::exists(temp_dir_)) {
        std::filesystem::remove_all(temp_dir_);
        spdlog::info("[TestData] cleaned up {}", temp_dir_);
    }
}

bool AnalysisTestData::generate_vbi_vbt() {
    spdlog::info("[TestData] generating VBI + VBT via AnalysisGenerator...");
    if (!vr::analysis::AnalysisGenerator::generate(kTestVideo, vbi_path_, vbt_path_)) {
        spdlog::error("[TestData] AnalysisGenerator::generate failed");
        return false;
    }
    spdlog::info("[TestData] VBI + VBT generated OK");
    return true;
}

bool AnalysisTestData::extract_raw_vvc() {
    spdlog::info("[TestData] extracting raw VVC via FFmpeg C API...");
    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, kTestVideo.c_str(), nullptr, nullptr);
    if (ret < 0) {
        spdlog::error("[TestData] avformat_open_input failed: {:#x}", static_cast<unsigned>(ret));
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        spdlog::error("[TestData] avformat_find_stream_info failed");
        avformat_close_input(&fmt_ctx);
        return false;
    }

    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }
    if (video_idx < 0) {
        spdlog::error("[TestData] no video stream found");
        avformat_close_input(&fmt_ctx);
        return false;
    }

    std::ofstream out(raw_vvc_path_, std::ios::binary);
    if (!out) {
        spdlog::error("[TestData] cannot create {}", raw_vvc_path_);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    // Try vvc_mp4toannexb bitstream filter
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
            } else {
                av_bsf_free(&bsf_ctx);
            }
        }
    }

    static const uint8_t kStartCode4[] = {0, 0, 0, 1};
    size_t total_written = 0;

    AVPacket* pkt = av_packet_alloc();
    while (true) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) break;
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }

        if (use_bsf) {
            ret = av_bsf_send_packet(bsf_ctx, pkt);
            if (ret < 0) { av_packet_unref(pkt); continue; }
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

    spdlog::info("[TestData] raw VVC extracted: {} bytes", total_written);
    return total_written > 0 && std::filesystem::exists(raw_vvc_path_);
}

bool AnalysisTestData::generate_vbs3() {
#ifdef _WIN32
    std::string decoder_path = VTM_DECODER_PATH;

    if (decoder_path.empty() || !std::filesystem::exists(decoder_path)) {
        spdlog::error("[TestData] VTM DecoderApp not found at: {} (build with: python dev.py vtm build)",
                      decoder_path.empty() ? "(VTM_DECODER_PATH not defined)" : decoder_path);
        return false;
    }

    spdlog::info("[TestData] generating VBS3 via VTM DecoderApp...");

    ScopedEnvVars env;
    env.set("VTM_BINARY_STATS", vbs3_path_);
    env.set("VTM_BINARY_STATS_FORMAT", "VBS3");

    std::string cmd = "\"" + decoder_path + "\" -b \"" + raw_vvc_path_ +
        "\" --TraceFile=NUL --TraceRule=\"D_BLOCK_STATISTICS_CODED:poc>=0\" -o NUL";

    int rc = run_command(cmd);
    spdlog::info("[TestData] VTM DecoderApp exit_code={}", rc);

    if (!std::filesystem::exists(vbs3_path_)) {
        spdlog::error("[TestData] VBS3 file not generated");
        return false;
    }

    auto size = std::filesystem::file_size(vbs3_path_);
    spdlog::info("[TestData] VBS3 generated: {} bytes", size);
    return true;
#else
    spdlog::error("[TestData] VBS3 generation only supported on Windows");
    return false;
#endif
}
