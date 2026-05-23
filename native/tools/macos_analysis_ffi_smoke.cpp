#include "analysis/analysis_ffi_abi.h"
#include "tools/test_video_assets.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

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

} // namespace

int main() {
    assert(naki_analysis_abi_version() == NAKI_ANALYSIS_ABI_VERSION);
    assert(naki_analysis_sizeof_summary() == sizeof(NakiAnalysisSummary));
    assert(naki_analysis_sizeof_frame_info() == sizeof(NakiFrameInfo));
    assert(naki_analysis_sizeof_nalu_info() == sizeof(NakiNaluInfo));
    assert(naki_analysis_sizeof_frame_bucket() == sizeof(NakiFrameBucket));
    assert(naki_analysis_sizeof_overlay_state() == sizeof(NakiOverlayState));

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
    assert(generated);

    const std::filesystem::path base_path = cache_root / hash / "base.vac";
    NakiAnalysisHandle handle = naki_analysis_open(base_path.string().c_str());
    assert(handle != nullptr);

    const NakiAnalysisSummary* summary = naki_analysis_handle_get_summary(handle);
    assert(summary != nullptr);
    assert(summary->loaded == 1);
    assert(summary->frame_count > 0);
    assert(summary->packet_count > 0);
    assert(summary->nalu_count > 0);
    assert(summary->video_width > 0);
    assert(summary->video_height > 0);

    NakiFrameInfo frame{};
    assert(naki_analysis_handle_get_frames_range(handle, 0, &frame, 1) == 1);
    assert(frame.pts != 0 || frame.dts != 0 || frame.keyframe != 0);

    NakiNaluInfo unit{};
    assert(naki_analysis_handle_get_nalus_range(handle, 0, &unit, 1) == 1);
    assert(unit.size > 0);

    NakiFrameBucket bucket{};
    assert(naki_analysis_handle_get_frame_buckets(handle, 0, 8, &bucket, 1) == 1);
    assert(bucket.frame_count > 0);

    naki_analysis_close(handle);
    std::filesystem::remove_all(cache_root);
    std::cout << "macos_analysis_ffi_smoke passed\n";
    return 0;
}
