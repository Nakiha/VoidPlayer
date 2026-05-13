#include "test_analysis_data.h"
#include "analysis/generators/analysis_generator.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <mutex>
#include <cstdlib>

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

#ifndef FFMPEG_ANALYZER_PATH
#define FFMPEG_ANALYZER_PATH ""
#endif

static const std::string kTestVideo = std::string(VIDEO_TEST_DIR) + "/h266_10s_1920x1080.mp4";
static const std::string kOverlayTestVideo = std::string(VIDEO_TEST_DIR) + "/h264_9s_1920x1080.mp4";

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
        vbs4_path_ = temp_dir_ + "/test.vbs4";
        vac2_base_path_ = temp_dir_ + "/base.vac";

        // Step 1: VBI + VBT via C++ AnalysisGenerator
        if (!generate_vbi_vbt()) return;

        // Step 2: VBS4 compatibility fixture via FFmpeg analyzer.
        if (!generate_vbs4()) return;

        // Step 3: VAC2 base cache
        if (!generate_vac2_base()) return;

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

bool AnalysisTestData::generate_vbs4() {
#ifdef _WIN32
    std::string analyzer_path = FFMPEG_ANALYZER_PATH;

    if (analyzer_path.empty() || !std::filesystem::exists(analyzer_path)) {
        spdlog::error("[TestData] FFmpeg analyzer not found at: {}",
                      analyzer_path.empty() ? "(FFMPEG_ANALYZER_PATH not defined)" : analyzer_path);
        return false;
    }
    if (!std::filesystem::exists(kOverlayTestVideo)) {
        spdlog::error("[TestData] overlay source video not found: {}", kOverlayTestVideo);
        return false;
    }

    spdlog::info("[TestData] generating VBS4 compatibility fixture via FFmpeg analyzer...");

    std::string cmd = "\"" + analyzer_path + "\" --codec h264 --input \"" +
        kOverlayTestVideo + "\" --vbs4 \"" + vbs4_path_ + "\"";

    int rc = run_command(cmd);
    spdlog::info("[TestData] FFmpeg analyzer exit_code={}", rc);
    if (rc != 0) {
        return false;
    }

    if (!std::filesystem::exists(vbs4_path_)) {
        spdlog::error("[TestData] VBS4 file not generated");
        return false;
    }

    auto size = std::filesystem::file_size(vbs4_path_);
    spdlog::info("[TestData] VBS4 generated: {} bytes", size);
    return true;
#else
    spdlog::error("[TestData] VBS4 generation only supported on Windows");
    return false;
#endif
}

bool AnalysisTestData::generate_vac2_base() {
    spdlog::info("[TestData] generating VAC2 base container...");
    if (!vr::analysis::AnalysisGenerator::generate_vac2_base(
            kTestVideo, vac2_base_path_)) {
        spdlog::error("[TestData] VAC2 base generation failed");
        return false;
    }
    if (!std::filesystem::exists(vac2_base_path_)) {
        spdlog::error("[TestData] VAC2 base not generated");
        return false;
    }
    spdlog::info("[TestData] VAC2 base generated: {} bytes",
                 std::filesystem::file_size(vac2_base_path_));
    return true;
}
