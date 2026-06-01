#include "analysis/analysis_ffi_abi.h"
#include "analysis/cache/overlay_chunk.h"
#include "analysis/cache/vacache_store.h"
#include "common/win_utf8.h"
#include "tools/test_video_assets.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

std::filesystem::path make_temp_cache_root() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ticks =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    std::ostringstream name;
    name << "voidplayer-macos-analysis-ffi-" << static_cast<long long>(getpid())
         << "-" << ticks;
    return std::filesystem::temp_directory_path() / name.str();
}

bool check(bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::cerr << message << "\n";
    return false;
}

} // namespace

int main() {
    if (!check(naki_analysis_abi_version() == NAKI_ANALYSIS_ABI_VERSION,
               "analysis ABI version mismatch") ||
        !check(naki_analysis_sizeof_summary() == sizeof(NakiAnalysisSummary),
               "analysis summary ABI size mismatch") ||
        !check(naki_analysis_sizeof_frame_info() == sizeof(NakiFrameInfo),
               "analysis frame info ABI size mismatch") ||
        !check(naki_analysis_sizeof_nalu_info() == sizeof(NakiNaluInfo),
               "analysis NALU info ABI size mismatch") ||
        !check(naki_analysis_sizeof_frame_bucket() == sizeof(NakiFrameBucket),
               "analysis frame bucket ABI size mismatch") ||
        !check(naki_analysis_sizeof_overlay_state() == sizeof(NakiOverlayState),
               "analysis overlay state ABI size mismatch")) {
        return 1;
    }

    const std::filesystem::path video =
        vp_tools::h264_smoke_video_path(VIDEO_TEST_DIR);
    const std::filesystem::path cache_root = make_temp_cache_root();
    std::filesystem::create_directories(cache_root);

    const std::string hash = "macos-analysis-ffi-smoke";
    const bool generated = naki_analysis_generate_vac2_base(
                               video.string().c_str(),
                               hash.c_str(),
                               cache_root.string().c_str(),
                               0) != 0;
    if (!check(generated, "failed to generate VAC2 base")) {
        return 1;
    }

    const std::filesystem::path base_path = cache_root / hash / "base.vac";
    NakiAnalysisHandle handle = naki_analysis_open(base_path.string().c_str());
    if (!check(handle != nullptr, "failed to open generated VAC2 base")) {
        return 1;
    }

    const NakiAnalysisSummary* summary = naki_analysis_handle_get_summary(handle);
    if (!check(summary != nullptr, "missing analysis summary") ||
        !check(summary->loaded == 1, "analysis summary was not loaded") ||
        !check(summary->frame_count > 0, "analysis summary has no frames") ||
        !check(summary->packet_count > 0, "analysis summary has no packets") ||
        !check(summary->nalu_count > 0, "analysis summary has no NAL units") ||
        !check(summary->video_width > 0, "analysis summary has invalid width") ||
        !check(summary->video_height > 0, "analysis summary has invalid height")) {
        naki_analysis_close(handle);
        return 1;
    }
    const int32_t frame_count = summary->frame_count;
    const int32_t codec = summary->codec;

    NakiFrameInfo frame{};
    if (!check(naki_analysis_handle_get_frames_range(handle, 0, &frame, 1) == 1,
               "failed to read analysis frame range") ||
        !check(frame.pts != 0 || frame.dts != 0 || frame.keyframe != 0,
               "analysis frame payload was empty")) {
        naki_analysis_close(handle);
        return 1;
    }

    NakiNaluInfo unit{};
    if (!check(naki_analysis_handle_get_nalus_range(handle, 0, &unit, 1) == 1,
               "failed to read analysis NALU range") ||
        !check(unit.size > 0, "analysis NALU payload was empty")) {
        naki_analysis_close(handle);
        return 1;
    }

    NakiFrameBucket bucket{};
    if (!check(naki_analysis_handle_get_frame_buckets(handle, 0, 8, &bucket, 1) == 1,
               "failed to read analysis frame buckets") ||
        !check(bucket.frame_count > 0, "analysis frame bucket was empty")) {
        naki_analysis_close(handle);
        return 1;
    }

    naki_analysis_close(handle);

    if (std::getenv("VOID_FFMPEG_ANALYZER") != nullptr && frame_count > 0) {
        const int32_t end_frame = std::min<int32_t>(2, frame_count - 1);
        if (!check(naki_analysis_generate_vac2_overlay_chunk(
                       video.string().c_str(),
                       hash.c_str(),
                       cache_root.string().c_str(),
                       0,
                       end_frame,
                       0) != 0,
                   "failed to generate overlay chunk")) {
            return 1;
        }

        const uint64_t job_id = naki_analysis_submit_vac2_overlay_chunk(
            video.string().c_str(),
            hash.c_str(),
            cache_root.string().c_str(),
            0,
            end_frame,
            0,
            0);
        if (!check(job_id != 0, "failed to submit overlay generation job")) {
            return 1;
        }
        NakiAnalysisGenerationJobResult job_result{};
        int32_t job_count = 0;
        for (int i = 0; i < 200 && job_count == 0; ++i) {
            job_count = naki_analysis_poll_generation_jobs(&job_result, 1);
            if (job_count == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (!check(job_count == 1, "overlay generation job did not complete") ||
            !check(job_result.job_id == job_id, "overlay generation job id mismatch") ||
            !check(job_result.ok != 0, "overlay generation job failed")) {
            return 1;
        }
        NakiAnalysisGenerationServiceStats service_stats{};
        naki_analysis_get_generation_service_stats(&service_stats);
        if (!check(service_stats.worker_count > 0, "generation service has no workers") ||
            !check(service_stats.submitted_jobs > 0, "generation service submitted no jobs")) {
            return 1;
        }

        vr::analysis::VacacheStore store(
            vr::win_utf8::path_to_utf8(cache_root),
            hash);
        vr::analysis::Vac2BaseFile base;
        if (!check(store.open_base(base), "failed to reopen VAC2 base")) {
            return 1;
        }

        vr::analysis::VachunkKey key;
        key.kind = VachunkKind::Overlay;
        key.codec = static_cast<AnalysisCodec>(codec);
        key.feature_flags =
            VACHUNK_FEATURE_CU_GEOMETRY |
            VACHUNK_FEATURE_QP |
            VACHUNK_FEATURE_PRED_MODE |
            VACHUNK_FEATURE_MOTION_VECTORS |
            VACHUNK_FEATURE_REF_INDEXES |
            VACHUNK_FEATURE_BIT_COST;
        key.base_content_revision = base.header().content_revision;
        key.generator_revision = 3;
        key.start_frame = 0;
        key.end_frame = static_cast<uint32_t>(end_frame);

        vr::analysis::VachunkFile chunk;
        if (!check(store.open_chunk(key, chunk), "failed to open overlay chunk")) {
            return 1;
        }
        vr::analysis::VachunkOverlayFrameData overlay_frame;
        if (!check(vr::analysis::read_overlay_vachunk_frame(chunk, 0, overlay_frame),
                   "failed to read overlay frame") ||
            !check(!overlay_frame.cus.empty(), "overlay frame had no CU records")) {
            return 1;
        }
    }

    std::filesystem::remove_all(cache_root);
    std::cout << "macos_analysis_ffi_smoke passed\n";
    return 0;
}
