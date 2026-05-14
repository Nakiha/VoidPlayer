#include <catch2/catch_test_macros.hpp>

#include "test_utils.h"
#include "video_renderer/track_lifecycle.h"
#include "video_renderer/track_pipeline_factory.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

TEST_CASE("TrackLifecycle removes and compacts track slots",
          "[track_pipeline][track_lifecycle]") {
    TrackPipelineManager manager;
    for (int i = 0; i < 3; ++i) {
        auto track = std::make_unique<TrackPipeline>();
        track->file_id = 10 + i;
        track->offset_us = i * 1000;
        track->track_buffer = std::make_shared<TrackBuffer>(1, 0);
        manager[static_cast<size_t>(i)] = std::move(track);
    }

    std::vector<int> unregistered_file_ids;
    std::vector<std::string> cleared_slots;
    std::vector<std::string> moved_slots;
    const TrackRemovalHooks hooks{
        [&](int file_id) {
            unregistered_file_ids.push_back(file_id);
        },
        [&](size_t slot, TrackPipeline& track) {
            cleared_slots.push_back(
                std::to_string(slot) + ":" + std::to_string(track.file_id));
        },
        [&](size_t from, size_t to, TrackPipeline& track) {
            moved_slots.push_back(
                std::to_string(from) + ">" + std::to_string(to) + ":" +
                std::to_string(track.file_id));
        },
    };

    remove_and_compact_track_pipeline(manager, 1, hooks);

    REQUIRE(unregistered_file_ids == std::vector<int>{11});
    REQUIRE(cleared_slots == std::vector<std::string>{"1:11"});
    REQUIRE(moved_slots == std::vector<std::string>{"2>1:12"});
    REQUIRE(manager[0]);
    REQUIRE(manager[0]->file_id == 10);
    REQUIRE(manager[1]);
    REQUIRE(manager[1]->file_id == 12);
    REQUIRE_FALSE(manager[2]);
}

TEST_CASE("TrackLifecycle compacts cached present decisions",
          "[track_pipeline][track_lifecycle]") {
    PresentDecision decision;
    TextureFrame frame0;
    frame0.pts_us = 1000;
    TextureFrame frame1;
    frame1.pts_us = 2000;
    TextureFrame frame2;
    frame2.pts_us = 3000;
    decision.frames[0] = frame0;
    decision.frames[1] = frame1;
    decision.frames[2] = frame2;

    compact_present_decision_frames(decision, 1);

    REQUIRE(decision.frames[0]);
    REQUIRE(decision.frames[0]->pts_us == 1000);
    REQUIRE(decision.frames[1]);
    REQUIRE(decision.frames[1]->pts_us == 3000);
    REQUIRE_FALSE(decision.frames[2]);
    REQUIRE_FALSE(decision.frames[3]);
}

TEST_CASE("TrackLifecycle computes track PTS end from demux stats",
          "[track_pipeline][track_lifecycle]") {
    DemuxStats no_duration;
    REQUIRE(track_pts_end_us_from_stats(no_duration) == 0);

    DemuxStats duration_span;
    duration_span.start_time_us = 1000;
    duration_span.duration_us = 4000;
    REQUIRE(track_pts_end_us_from_stats(duration_span) == 5000);

    DemuxStats absolute_end;
    absolute_end.start_time_us = 3000;
    absolute_end.duration_us = 5000;
    REQUIRE(track_pts_end_us_from_stats(absolute_end) == 5000);
}

TEST_CASE("TrackLifecycle computes duration cache",
          "[track_pipeline][track_lifecycle]") {
    TrackPipeline no_demux_track;
    REQUIRE(track_duration_us(no_demux_track) == 0);
    REQUIRE(extend_track_duration_cache(5000, no_demux_track) == 5000);

    TrackPipelineManager manager;
    auto empty_track = std::make_unique<TrackPipeline>();
    manager[0] = std::move(empty_track);
    REQUIRE(compute_track_duration_cache(manager) == 0);

    TrackPipelineFactory factory;
    auto pipeline = factory.create_opened_pipeline(
        video_test_dir() + "/h264_9s_1920x1080.mp4",
        false);
    REQUIRE(pipeline);
    const int64_t duration_us = track_duration_us(*pipeline);
    REQUIRE(duration_us > 0);
    REQUIRE(extend_track_duration_cache(1, *pipeline) == duration_us);

    manager[1] = std::move(pipeline);
    REQUIRE(compute_track_duration_cache(manager) == duration_us);
    manager.stop_slot(1);
}

TEST_CASE("TrackLifecycle prepares add-track seek to current clock",
          "[track_pipeline][track_lifecycle]") {
    TrackPipeline track;
    track.file_id = 42;
    track.offset_us = 250000;
    track.packet_queue = std::make_unique<PacketQueue>();
    track.audio_packet_queue = std::make_unique<PacketQueue>();
    track.track_buffer = std::make_shared<TrackBuffer>(4, 1);
    track.seek_controller = std::make_unique<SeekController>();
    track.track_buffer->set_state(TrackState::Ready);

    TextureFrame frame;
    frame.pts_us = 1000;
    track.track_buffer->push_frame(frame);
    REQUIRE(track.track_buffer->total_count() == 1);

    int audio_pause_count = 0;
    int paused_file_id = -1;
    bool paused_value = false;
    const TrackAddSeekHooks hooks{
        [&](int file_id, bool paused) {
            ++audio_pause_count;
            paused_file_id = file_id;
            paused_value = paused;
        },
    };

    const auto idle_result =
        prepare_add_track_seek_to_clock(track, 0, true, hooks);
    REQUIRE_FALSE(idle_result.applied);
    REQUIRE(track.track_buffer->total_count() == 1);
    REQUIRE_FALSE(track.seek_controller->has_pending_seek());
    REQUIRE(audio_pause_count == 0);

    const auto paused_result =
        prepare_add_track_seek_to_clock(track, 1000000, false, hooks);
    REQUIRE(paused_result.applied);
    REQUIRE(paused_result.target_pts_us == 750000);
    REQUIRE(paused_result.seek_type == SeekType::Exact);
    REQUIRE(track.track_buffer->state() == TrackState::Buffering);
    REQUIRE(track.track_buffer->total_count() == 0);
    REQUIRE(audio_pause_count == 1);
    REQUIRE(paused_file_id == 42);
    REQUIRE(paused_value);

    auto pending_seek = track.seek_controller->take_pending();
    REQUIRE(pending_seek);
    REQUIRE(pending_seek->target_pts_us == 750000);
    REQUIRE(pending_seek->type == SeekType::Exact);
    REQUIRE(track.packet_queue->try_pop().status == PacketPopStatus::Flushed);
    REQUIRE(track.audio_packet_queue->try_pop().status == PacketPopStatus::Flushed);

    const auto playing_result =
        prepare_add_track_seek_to_clock(track, 2000000, true, hooks);
    REQUIRE(playing_result.applied);
    REQUIRE(playing_result.target_pts_us == 1750000);
    REQUIRE(playing_result.seek_type == SeekType::Keyframe);
    pending_seek = track.seek_controller->take_pending();
    REQUIRE(pending_seek);
    REQUIRE(pending_seek->target_pts_us == 1750000);
    REQUIRE(pending_seek->type == SeekType::Keyframe);
    REQUIRE(audio_pause_count == 2);
}

TEST_CASE("TrackLifecycle commits new track pipeline slot",
          "[track_pipeline][track_lifecycle]") {
    TrackPipelineManager manager;
    auto pipeline = std::make_unique<TrackPipeline>();
    pipeline->file_id = 55;
    pipeline->offset_us = 12345;
    pipeline->track_buffer = std::make_shared<TrackBuffer>(1, 0);

    std::vector<std::string> events;
    const TrackAddCommitHooks hooks{
        [&](size_t slot, TrackPipeline& track) {
            events.push_back(
                "render:" + std::to_string(slot) + ":" +
                std::to_string(track.file_id));
        },
        [&](size_t slot) {
            events.push_back("reset:" + std::to_string(slot));
        },
    };

    TrackPipeline* committed =
        commit_new_track_pipeline(manager, 1, std::move(pipeline), hooks);

    REQUIRE(committed != nullptr);
    REQUIRE(manager[1].get() == committed);
    REQUIRE(committed->file_id == 55);
    REQUIRE(committed->offset_us == 12345);
    REQUIRE(events == std::vector<std::string>{"render:1:55", "reset:1"});

    REQUIRE_FALSE(commit_new_track_pipeline(
        manager, kMaxTracks, std::unique_ptr<TrackPipeline>{}, hooks));
}

TEST_CASE("TrackLifecycle prepares generic track seek transition",
          "[track_pipeline][track_lifecycle]") {
    TrackPipeline track;
    track.file_id = 77;
    track.packet_queue = std::make_unique<PacketQueue>();
    track.audio_packet_queue = std::make_unique<PacketQueue>();
    track.track_buffer = std::make_shared<TrackBuffer>(4, 1);
    track.track_buffer->set_state(TrackState::Ready);

    TextureFrame frame;
    frame.pts_us = 1000;
    track.track_buffer->push_frame(frame);

    int audio_pause_count = 0;
    int presenter_reset_count = 0;
    const TrackSeekPreparationHooks hooks{
        [&](int file_id, bool paused) {
            REQUIRE(file_id == 77);
            REQUIRE(paused);
            ++audio_pause_count;
        },
        [&]() {
            ++presenter_reset_count;
        },
    };

    const TrackSeekPreparationConfig config{
        true,
    };
    const auto result = prepare_track_seek_transition(track, config, hooks);

    REQUIRE(result.buffered_frames_before == 1);
    REQUIRE(result.packet_queue_size_before == 0);
    REQUIRE(result.buffer_state_before == TrackState::Ready);
    REQUIRE_FALSE(result.seek_transition_active);
    REQUIRE(track.track_buffer->state() == TrackState::Flushing);
    REQUIRE(track.track_buffer->total_count() == 0);
    REQUIRE(track.packet_queue->try_pop().status == PacketPopStatus::Flushed);
    REQUIRE(track.audio_packet_queue->try_pop().status == PacketPopStatus::Flushed);
    REQUIRE(audio_pause_count == 1);
    REQUIRE(presenter_reset_count == 1);

    const auto transition_result =
        prepare_track_seek_transition(track, TrackSeekPreparationConfig{}, hooks);
    REQUIRE(transition_result.buffer_state_before == TrackState::Flushing);
    REQUIRE(transition_result.seek_transition_active);
}

TEST_CASE("TrackLifecycle submits seek after optional recreate",
          "[track_pipeline][track_lifecycle]") {
    TrackPipeline track;
    track.seek_controller = std::make_unique<SeekController>();

    submit_track_seek_after_recreate(
        track, 123000, SeekType::Exact, true, false);
    auto pending_seek = track.seek_controller->take_pending();
    REQUIRE(pending_seek);
    REQUIRE(pending_seek->target_pts_us == 123000);
    REQUIRE(pending_seek->type == SeekType::Exact);

    submit_track_seek_after_recreate(
        track, 456000, SeekType::Keyframe, false, true);
    REQUIRE_FALSE(track.seek_controller->has_pending_seek());
}

TEST_CASE("TrackLifecycle recreates track pipeline for seek",
          "[track_pipeline][track_lifecycle]") {
    TrackPipelineFactory factory;
    const std::string path = video_test_dir() + "/h264_9s_1920x1080.mp4";
    auto current = factory.create_opened_pipeline(path, false);
    REQUIRE(current);
    current->file_id = 91;
    current->offset_us = 12345;

    TrackPipelineManager manager;
    manager[0] = std::move(current);

    std::atomic<int> seek_callbacks{0};
    std::vector<std::string> events;
    TrackPipelineRecreateHooks hooks;
    hooks.unregister_audio = [&](int file_id) {
        events.push_back("unregister:" + std::to_string(file_id));
    };
    hooks.clear_slot = [&](size_t slot) {
        events.push_back("clear:" + std::to_string(slot));
    };
    hooks.reset_presenter_track = [&](size_t slot) {
        events.push_back("reset:" + std::to_string(slot));
    };
    hooks.create_pipeline =
        [&](const std::string& recreate_path,
            bool use_hardware_decode,
            const SeekRequest& initial_seek) {
            REQUIRE(recreate_path == path);
            REQUIRE_FALSE(use_hardware_decode);
            REQUIRE(initial_seek.target_pts_us == 1000000);
            REQUIRE(initial_seek.type == SeekType::Exact);
            events.push_back("create");
            return factory.create_opened_pipeline(
                recreate_path, use_hardware_decode, &initial_seek);
        };
    hooks.start_hooks.configure_seek_callback = [&](TrackPipeline& track) {
        events.push_back("configure_seek");
        track.demux_thread->set_seek_callback(
            [&](int64_t, SeekType) {
                seek_callbacks.fetch_add(1, std::memory_order_acq_rel);
            });
    };
    hooks.start_hooks.configure_error_callback = [&](TrackPipeline&) {
        events.push_back("configure_error");
    };
    hooks.start_hooks.register_audio = [&](TrackPipeline& track) {
        events.push_back("register:" + std::to_string(track.file_id));
    };
    hooks.start_hooks.unregister_audio = [&](int file_id) {
        events.push_back("start_unregister:" + std::to_string(file_id));
    };
    hooks.commit_slot = [&](size_t slot, TrackPipeline& track) {
        events.push_back(
            "commit:" + std::to_string(slot) + ":" + std::to_string(track.file_id));
    };

    REQUIRE(recreate_track_pipeline_for_seek(
        manager, 0, 1000000, SeekType::Exact, hooks, "TrackLifecycleTest"));

    REQUIRE(manager[0]);
    REQUIRE(manager[0]->file_id == 91);
    REQUIRE(manager[0]->offset_us == 12345);
    REQUIRE(manager[0]->recreated_for_paused_hevc_seek);
    REQUIRE(events == std::vector<std::string>{
        "unregister:91",
        "clear:0",
        "reset:0",
        "create",
        "configure_seek",
        "configure_error",
        "register:91",
        "commit:0:91",
    });

    manager.stop_slot(0);
    (void)seek_callbacks.load(std::memory_order_acquire);
}
