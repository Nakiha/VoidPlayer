#include <catch2/catch_test_macros.hpp>
#include "analysis_ffi.h"
#include "test_analysis_data.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

TEST_CASE("analysis FFI handle returns empty data after close",
          "[analysis][ffi]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    NakiAnalysisHandle handle = naki_analysis_open(
        data.vbs2_path().c_str(),
        data.vbi_path().c_str(),
        data.vbt_path().c_str());
    REQUIRE(handle != nullptr);

    const NakiAnalysisSummary* loaded = naki_analysis_handle_get_summary(handle);
    REQUIRE(loaded != nullptr);
    REQUIRE(loaded->loaded == 1);
    REQUIRE(loaded->frame_count > 0);

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

TEST_CASE("analysis FFI handle close is safe while readers are active",
          "[analysis][ffi][concurrency]") {
    auto& data = AnalysisTestData::instance();
    REQUIRE(data.ensure());

    NakiAnalysisHandle handle = naki_analysis_open(
        data.vbs2_path().c_str(),
        data.vbi_path().c_str(),
        data.vbt_path().c_str());
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
