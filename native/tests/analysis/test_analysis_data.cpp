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

        vac2_base_path_ = temp_dir_ + "/base.vac";

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
