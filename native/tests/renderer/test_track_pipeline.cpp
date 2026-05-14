#include <catch2/catch_test_macros.hpp>

#include "test_utils.h"
#include "video_renderer/track_pipeline_factory.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace vr;
using namespace vr::test;

TEST_CASE("TrackPipelineFactory creates opened pipeline before demux worker start",
          "[track_pipeline]") {
    TrackPipelineFactory factory;
    auto pipeline = factory.create_opened_pipeline(
        video_test_dir() + "/h264_9s_1920x1080.mp4",
        false);

    REQUIRE(pipeline);
    REQUIRE(pipeline->seek_controller);
    REQUIRE(pipeline->packet_queue);
    REQUIRE(pipeline->audio_packet_queue);
    REQUIRE(pipeline->demux_thread);
    REQUIRE(pipeline->decode_thread);
    REQUIRE(pipeline->track_buffer);

    const auto& stats = pipeline->demux_thread->stats();
    REQUIRE(stats.video_stream_index >= 0);
    REQUIRE(stats.codec_params != nullptr);
    REQUIRE(pipeline->video_width == stats.width);
    REQUIRE(pipeline->video_height == stats.height);
    REQUIRE(pipeline->video_aspect > 0.0f);

    std::atomic<int> seek_callbacks{0};
    std::atomic<int64_t> callback_pts{-1};
    pipeline->demux_thread->set_seek_callback(
        [&](int64_t pts, SeekType type) {
            if (type == SeekType::Keyframe) {
                callback_pts.store(pts, std::memory_order_release);
                seek_callbacks.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    pipeline->seek_controller->request_seek(1000000, SeekType::Keyframe);

    REQUIRE(pipeline->demux_thread->start_thread());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           seek_callbacks.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TrackPipelineManager manager;
    manager[0] = std::move(pipeline);
    manager.stop_slot(0);

    REQUIRE(seek_callbacks.load(std::memory_order_acquire) == 1);
    REQUIRE(callback_pts.load(std::memory_order_acquire) == 1000000);
}
