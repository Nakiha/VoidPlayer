#include <catch2/catch_test_macros.hpp>

#include "test_utils.h"
#include "video_renderer/track_lifecycle.h"
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

TEST_CASE("TrackLifecycle configures and starts pipeline with rollback hooks",
          "[track_pipeline][track_lifecycle]") {
    TrackPipelineFactory factory;
    auto pipeline = factory.create_opened_pipeline(
        video_test_dir() + "/h264_9s_1920x1080.mp4",
        false);

    REQUIRE(pipeline);

    std::atomic<int> configured_seek_callbacks{0};
    std::atomic<int> configured_error_callbacks{0};
    std::atomic<int> audio_registers{0};
    std::atomic<int> audio_unregisters{0};
    std::atomic<int> seek_callbacks{0};
    std::atomic<int64_t> callback_pts{-1};

    TrackPipelineStartHooks hooks;
    hooks.configure_seek_callback = [&](TrackPipeline& track) {
        configured_seek_callbacks.fetch_add(1, std::memory_order_acq_rel);
        track.demux_thread->set_seek_callback(
            [&](int64_t pts, SeekType type) {
                if (type == SeekType::Exact) {
                    callback_pts.store(pts, std::memory_order_release);
                    seek_callbacks.fetch_add(1, std::memory_order_acq_rel);
                }
            });
    };
    hooks.configure_error_callback = [&](TrackPipeline&) {
        configured_error_callbacks.fetch_add(1, std::memory_order_acq_rel);
    };
    hooks.register_audio = [&](TrackPipeline&) {
        audio_registers.fetch_add(1, std::memory_order_acq_rel);
    };
    hooks.unregister_audio = [&](int) {
        audio_unregisters.fetch_add(1, std::memory_order_acq_rel);
    };

    const TrackPipelineStartConfig start_config{
        7,
        12345,
        true,
        true,
    };
    REQUIRE(configure_and_start_track_pipeline(
        *pipeline, start_config, hooks, "TrackLifecycleTest"));

    REQUIRE(pipeline->file_id == 7);
    REQUIRE(pipeline->offset_us == 12345);
    REQUIRE(pipeline->recreated_for_paused_hevc_seek);
    REQUIRE(configured_seek_callbacks.load(std::memory_order_acquire) == 1);
    REQUIRE(configured_error_callbacks.load(std::memory_order_acquire) == 1);
    REQUIRE(audio_registers.load(std::memory_order_acquire) == 1);
    REQUIRE(audio_unregisters.load(std::memory_order_acquire) == 0);

    pipeline->seek_controller->request_seek(1000000, SeekType::Exact);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           seek_callbacks.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TrackPipelineManager manager;
    manager[0] = std::move(pipeline);
    manager.stop_slot(0, [&](size_t, TrackPipeline& track) {
        hooks.unregister_audio(track.file_id);
    });

    REQUIRE(seek_callbacks.load(std::memory_order_acquire) == 1);
    REQUIRE(callback_pts.load(std::memory_order_acquire) == 1000000);
    REQUIRE(audio_unregisters.load(std::memory_order_acquire) == 1);
}
