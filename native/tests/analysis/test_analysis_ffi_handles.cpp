#include <catch2/catch_test_macros.hpp>
#include "analysis_ffi.h"
#include "analysis/parsers/vac2_parser.h"
#include "test_analysis_data.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

TEST_CASE("analysis FFI exposes ABI version and struct sizes",
          "[analysis][ffi][abi]") {
    REQUIRE(naki_analysis_abi_version() == 1);
    REQUIRE(naki_analysis_sizeof_summary() == sizeof(NakiAnalysisSummary));
    REQUIRE(naki_analysis_sizeof_frame_info() == sizeof(NakiFrameInfo));
    REQUIRE(naki_analysis_sizeof_nalu_info() == sizeof(NakiNaluInfo));
    REQUIRE(naki_analysis_sizeof_frame_bucket() == sizeof(NakiFrameBucket));
    REQUIRE(naki_analysis_sizeof_overlay_state() == sizeof(NakiOverlayState));
    REQUIRE(naki_analysis_sizeof_summary_v2() == sizeof(NakiAnalysisSummaryV2));
    REQUIRE(naki_analysis_sizeof_frame_info_v2() == sizeof(NakiFrameInfoV2));
    REQUIRE(naki_analysis_sizeof_nalu_info_v2() == sizeof(NakiNaluInfoV2));
    REQUIRE(naki_analysis_sizeof_frame_bucket_v2() == sizeof(NakiFrameBucketV2));
    REQUIRE(naki_analysis_sizeof_overlay_state_v2() == sizeof(NakiOverlayStateV2));
    REQUIRE(sizeof(NakiAnalysisSummaryV2) >
            sizeof(NakiAnalysisStructHeader) + sizeof(NakiAnalysisSummary) - 1);
}

TEST_CASE("analysis FFI loads and clears overlay tracks",
          "[analysis][ffi]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    REQUIRE(naki_analysis_set_overlay_track(7, data.vac2_base_path().c_str()) == 1);
    naki_analysis_clear_overlay_tracks();
    REQUIRE(naki_analysis_set_overlay_track(-1, data.vac2_base_path().c_str()) == 0);
    naki_analysis_clear_overlay_tracks();
}

TEST_CASE("analysis FFI handle returns empty data after close",
          "[analysis][ffi]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    NakiAnalysisHandle handle = naki_analysis_open(data.vac2_base_path().c_str());
    REQUIRE(handle != nullptr);

    const NakiAnalysisSummary* loaded = naki_analysis_handle_get_summary(handle);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->loaded == 1);
    REQUIRE(loaded->frame_count > 0);
    REQUIRE(loaded->current_frame_idx == -1);

    naki_analysis_close(handle);
    naki_analysis_close(handle);

    const NakiAnalysisSummary* closed = naki_analysis_handle_get_summary(handle);
    REQUIRE(closed != nullptr);
    REQUIRE(closed->loaded == 0);

    NakiFrameInfo frame{};
    NakiNaluInfo nalu{};
    REQUIRE(naki_analysis_handle_get_frames_range(handle, 0, &frame, 1) == 0);
    REQUIRE(naki_analysis_handle_get_nalus_range(handle, 0, &nalu, 1) == 0);
}

TEST_CASE("analysis FFI handle opens VAC2 base indexes",
          "[analysis][ffi][vac2]") {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "voidplayer_ffi_handle_base.vac";
    fs::remove(path);

    vr::analysis::Vac2BaseData data;
    data.codec = VbiCodec::H264;
    data.time_base_num = 1;
    data.time_base_den = 1000;
    data.width = 1920;
    data.height = 1080;
    data.metadata_json = R"({"schema":"ffi-vac2-test"})";

    Vac2PacketEntry packet0{};
    packet0.pts = 100;
    packet0.dts = 90;
    packet0.duration = 40;
    packet0.size = 1200;
    packet0.flags = VAC2_PACKET_FLAG_KEYFRAME;
    packet0.file_offset = UINT64_MAX;
    packet0.format_offset = UINT64_MAX;
    packet0.first_unit = 0;
    packet0.unit_count = 1;
    packet0.au_index = 0;
    data.packets.push_back(packet0);

    Vac2PacketEntry packet1 = packet0;
    packet1.pts = 140;
    packet1.dts = 130;
    packet1.size = 900;
    packet1.flags = 0;
    packet1.first_unit = 1;
    packet1.au_index = 1;
    data.packets.push_back(packet1);

    Vac2BitstreamUnitEntry unit0{};
    unit0.packet_index = 0;
    unit0.au_index = 0;
    unit0.offset = 11;
    unit0.size = 100;
    unit0.nal_type = 5;
    unit0.temporal_id = 0;
    unit0.flags = VAC2_UNIT_FLAG_IS_VCL |
                  VAC2_UNIT_FLAG_IS_SLICE |
                  VAC2_UNIT_FLAG_IS_KEYFRAME;
    unit0.pset_snapshot = UINT16_MAX;
    data.units.push_back(unit0);

    Vac2BitstreamUnitEntry unit1 = unit0;
    unit1.packet_index = 1;
    unit1.au_index = 1;
    unit1.offset = 111;
    unit1.size = 80;
    unit1.nal_type = 1;
    unit1.flags = VAC2_UNIT_FLAG_IS_VCL | VAC2_UNIT_FLAG_IS_SLICE;
    data.units.push_back(unit1);

    Vac2FrameEntry frame0{};
    frame0.first_packet = 0;
    frame0.packet_count = 1;
    frame0.first_unit = 0;
    frame0.unit_count = 1;
    frame0.pts = 100;
    frame0.dts = 90;
    frame0.duration = 40;
    frame0.coded_order = 0;
    frame0.display_order = 0;
    frame0.poc = 0;
    frame0.frame_size = 1200;
    frame0.flags = VAC2_FRAME_FLAG_KEYFRAME | VAC2_FRAME_FLAG_RAP;
    data.frames.push_back(frame0);

    Vac2FrameEntry frame1 = frame0;
    frame1.first_packet = 1;
    frame1.first_unit = 1;
    frame1.pts = 140;
    frame1.dts = 130;
    frame1.coded_order = 1;
    frame1.display_order = 2;
    frame1.poc = 2;
    frame1.frame_size = 900;
    frame1.flags = 0;
    data.frames.push_back(frame1);

    Vac2FrameSummaryEntry summary0{};
    summary0.poc = 0;
    summary0.coded_order = 0;
    summary0.first_vcl_unit = 0;
    summary0.temporal_id = 0;
    summary0.slice_type = 2;
    summary0.nal_type = 5;
    summary0.qp_kind = VAC2_QP_KIND_SLICE;
    summary0.qp_avg = 22;
    data.frame_summaries.push_back(summary0);

    Vac2FrameSummaryEntry summary1 = summary0;
    summary1.poc = 2;
    summary1.coded_order = 1;
    summary1.first_vcl_unit = 1;
    summary1.slice_type = 1;
    summary1.nal_type = 1;
    summary1.qp_avg = 27;
    summary1.num_ref_l0 = 1;
    summary1.ref_pocs_l0[0] = 0;
    data.frame_summaries.push_back(summary1);

    REQUIRE(vr::analysis::write_vac2_base_container(path.string(), data));

    NakiAnalysisHandle handle = naki_analysis_open(path.string().c_str());
    REQUIRE(handle != nullptr);

    const auto* summary = naki_analysis_handle_get_summary(handle);
    REQUIRE(summary != nullptr);
    REQUIRE(summary->loaded == 1);
    REQUIRE(summary->frame_count == 2);
    REQUIRE(summary->packet_count == 2);
    REQUIRE(summary->nalu_count == 2);
    REQUIRE(summary->codec == static_cast<int32_t>(VbiCodec::H264));

    NakiFrameInfo frame{};
    REQUIRE(naki_analysis_handle_get_frames_range(handle, 1, &frame, 1) == 1);
    REQUIRE(frame.poc == 2);
    REQUIRE(frame.avg_qp == 27);
    REQUIRE(frame.num_ref_l0 == 1);
    REQUIRE(frame.ref_pocs_l0[0] == 0);
    REQUIRE(frame.packet_size == 900);
    REQUIRE(frame.keyframe == 0);

    NakiNaluInfo nalu{};
    REQUIRE(naki_analysis_handle_get_nalus_range(handle, 0, &nalu, 1) == 1);
    REQUIRE(nalu.offset == 11);
    REQUIRE(nalu.nal_type == 5);
    REQUIRE((nalu.flags & 0x07) == 0x07);

    REQUIRE(naki_analysis_handle_frame_to_nalu(handle, 1) == 1);
    REQUIRE(naki_analysis_handle_nalu_to_frame(handle, 1) == 1);

    NakiFrameBucket bucket{};
    REQUIRE(naki_analysis_handle_get_frame_buckets(handle, 0, 2, &bucket, 1) == 1);
    REQUIRE(bucket.frame_count == 2);
    REQUIRE(bucket.packet_size_min == 900);
    REQUIRE(bucket.packet_size_max == 1200);
    REQUIRE(bucket.keyframe_count == 1);

    naki_analysis_close(handle);
    fs::remove(path);
}

TEST_CASE("analysis FFI generates VAC2 base cache layout",
          "[analysis][ffi][vac2]") {
    namespace fs = std::filesystem;
    const std::string video_path =
        std::string(VIDEO_TEST_DIR) + "/h264_9s_1920x1080.mp4";
    REQUIRE(fs::exists(video_path));

    const fs::path cache_root =
        fs::temp_directory_path() / "voidplayer_ffi_vac2_cache";
    fs::remove_all(cache_root);
    REQUIRE(fs::create_directories(cache_root));

    REQUIRE(naki_analysis_generate_vac2_base(
                video_path.c_str(),
                "ffi_vac2",
                cache_root.string().c_str(),
                64 * 1024 * 1024) == 1);

    const fs::path base_path = cache_root / "ffi_vac2" / "base.vac";
    REQUIRE(fs::exists(base_path));

    NakiAnalysisHandle handle = naki_analysis_open(base_path.string().c_str());
    REQUIRE(handle != nullptr);
    const auto* summary = naki_analysis_handle_get_summary(handle);
    REQUIRE(summary != nullptr);
    REQUIRE(summary->loaded == 1);
    REQUIRE(summary->frame_count > 0);
    REQUIRE(summary->packet_count > 0);
    REQUIRE(summary->nalu_count > 0);
    naki_analysis_close(handle);

    fs::remove_all(cache_root);
}

TEST_CASE("analysis FFI rejects overlay chunk generation without VAC2 base",
          "[analysis][ffi][vac2][vachunk]") {
    namespace fs = std::filesystem;
    const std::string video_path =
        std::string(VIDEO_TEST_DIR) + "/h264_9s_1920x1080.mp4";
    REQUIRE(fs::exists(video_path));

    const fs::path cache_root =
        fs::temp_directory_path() / "voidplayer_ffi_vac2_overlay_cache";
    fs::remove_all(cache_root);
    REQUIRE(fs::create_directories(cache_root));

    REQUIRE(naki_analysis_generate_vac2_overlay_chunk(
                video_path.c_str(),
                "ffi_overlay",
                cache_root.string().c_str(),
                0,
                0,
                128 * 1024 * 1024) == 0);
    char message[128] = {};
    REQUIRE(naki_analysis_last_error(message, sizeof(message)) ==
            NAKI_ANALYSIS_ERR_OPEN_FAILED);
    REQUIRE(std::string(message).find("base") != std::string::npos);

    fs::remove_all(cache_root);
}

TEST_CASE("analysis FFI handle close is safe while readers are active",
          "[analysis][ffi][concurrency]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    NakiAnalysisHandle handle = naki_analysis_open(data.vac2_base_path().c_str());
    REQUIRE(handle != nullptr);

    std::atomic<bool> stop{false};
    std::atomic<int> loaded_reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&] {
            std::vector<NakiFrameInfo> frames(16);
            std::vector<NakiNaluInfo> nalus(16);
            while (!stop.load(std::memory_order_acquire)) {
                const auto* summary = naki_analysis_handle_get_summary(handle);
                if (summary && summary->loaded) {
                    loaded_reads.fetch_add(1, std::memory_order_relaxed);
                }
                (void)naki_analysis_handle_get_frames_range(
                    handle, 0, frames.data(), static_cast<int32_t>(frames.size()));
                (void)naki_analysis_handle_get_nalus_range(
                    handle, 0, nalus.data(), static_cast<int32_t>(nalus.size()));
            }
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (loaded_reads.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(loaded_reads.load(std::memory_order_relaxed) > 0);

    naki_analysis_close(handle);
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) {
        reader.join();
    }

    const auto* summary = naki_analysis_handle_get_summary(handle);
    REQUIRE(summary != nullptr);
    REQUIRE(summary->loaded == 0);
}

TEST_CASE("analysis FFI reports thread-local last errors",
          "[analysis][ffi][abi]") {
    char message[128] = {};
    REQUIRE(naki_analysis_open(nullptr) == nullptr);
    REQUIRE(naki_analysis_last_error(message, sizeof(message)) ==
            NAKI_ANALYSIS_ERR_INVALID_ARGUMENT);
    REQUIRE(std::string(message).find("analysis_path") != std::string::npos);

    REQUIRE(naki_analysis_handle_get_frames_range(
                nullptr, 0, nullptr, 1) == 0);
    REQUIRE(naki_analysis_last_error(message, sizeof(message)) ==
            NAKI_ANALYSIS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("analysis FFI handle exposes frame mappings and buckets",
          "[analysis][ffi]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    NakiAnalysisHandle handle = naki_analysis_open(data.vac2_base_path().c_str());
    REQUIRE(handle != nullptr);

    const auto* summary = naki_analysis_handle_get_summary(handle);
    REQUIRE(summary != nullptr);
    REQUIRE(summary->loaded == 1);
    REQUIRE(summary->frame_count > 4);
    REQUIRE(summary->nalu_count > 4);

    const int32_t nalu0 = naki_analysis_handle_frame_to_nalu(handle, 0);
    REQUIRE(nalu0 >= 0);
    REQUIRE(naki_analysis_handle_nalu_to_frame(handle, nalu0) == 0);
    REQUIRE(naki_analysis_handle_frame_to_nalu(handle, -1) == -1);
    REQUIRE(naki_analysis_handle_nalu_to_frame(handle, -1) == -1);

    std::vector<NakiFrameBucket> buckets(4);
    const int32_t bucket_count = naki_analysis_handle_get_frame_buckets(
        handle, 0, 2, buckets.data(), static_cast<int32_t>(buckets.size()));
    REQUIRE(bucket_count > 0);
    REQUIRE(buckets[0].start_frame == 0);
    REQUIRE(buckets[0].frame_count > 0);
    REQUIRE(buckets[0].packet_size_max >= buckets[0].packet_size_min);
    REQUIRE(buckets[0].packet_size_sum >= buckets[0].packet_size_min);

    naki_analysis_close(handle);
    REQUIRE(naki_analysis_handle_frame_to_nalu(handle, 0) == -1);
    REQUIRE(naki_analysis_handle_nalu_to_frame(handle, nalu0) == -1);
    REQUIRE(naki_analysis_handle_get_frame_buckets(
        handle, 0, 2, buckets.data(), static_cast<int32_t>(buckets.size())) == 0);
}
