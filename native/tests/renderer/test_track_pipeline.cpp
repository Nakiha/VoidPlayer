#include <catch2/catch_test_macros.hpp>

#include "test_utils.h"
#include "video_renderer/layout_controller.h"
#include "video_renderer/layout_geometry.h"
#include "video_renderer/track_lifecycle.h"
#include "video_renderer/track_pipeline_factory.h"
#include "video_renderer/track_preroll_policy.h"
#include "video_renderer/track_present_policy.h"
#include "video_renderer/track_preview_policy.h"
#include "video_renderer/track_snapshot.h"
#include "video_renderer/track_step_policy.h"

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <optional>
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

TEST_CASE("TrackPipelineManager exposes active track queries",
          "[track_pipeline]") {
    TrackPipelineManager manager;
    REQUIRE(manager.count() == 0);
    REQUIRE(manager.first_active_slot() == -1);

    auto first = std::make_unique<TrackPipeline>();
    first->file_id = 21;
    manager[1] = std::move(first);
    auto second = std::make_unique<TrackPipeline>();
    second->file_id = 42;
    manager[3] = std::move(second);

    REQUIRE(manager.count() == 2);
    REQUIRE(manager.first_active_slot() == 1);
    REQUIRE(manager.find_slot_by_file_id(42) == 3);

    manager.stop_slot(1);
    REQUIRE(manager.count() == 1);
    REQUIRE(manager.first_active_slot() == 3);

    manager.clear();
    REQUIRE(manager.count() == 0);
    REQUIRE(manager.first_active_slot() == -1);
}

TEST_CASE("LayoutGeometry snapshots track geometry",
          "[track_pipeline][layout_geometry]") {
    TrackPipelineManager empty_manager;
    auto empty = snapshot_layout_track_geometry(empty_manager);
    for (const auto& track : empty) {
        REQUIRE_FALSE(track.active);
        REQUIRE(track.width == 0);
        REQUIRE(track.height == 0);
        REQUIRE(track.aspect == 1.0f);
    }

    TrackPipelineManager manager;
    auto primary = std::make_unique<TrackPipeline>();
    primary->video_width = 1920;
    primary->video_height = 1080;
    primary->video_aspect = 16.0f / 9.0f;
    manager[0] = std::move(primary);

    auto secondary = std::make_unique<TrackPipeline>();
    secondary->video_width = 1280;
    secondary->video_height = 720;
    secondary->video_aspect = 4.0f / 3.0f;
    manager[2] = std::move(secondary);

    auto geometry = snapshot_layout_track_geometry(manager);
    REQUIRE(geometry[0].active);
    REQUIRE(geometry[0].width == 1920);
    REQUIRE(geometry[0].height == 1080);
    REQUIRE(geometry[0].aspect == 16.0f / 9.0f);
    REQUIRE_FALSE(geometry[1].active);
    REQUIRE(geometry[2].active);
    REQUIRE(geometry[2].width == 1280);
    REQUIRE(geometry[2].height == 720);
    REQUIRE(geometry[2].aspect == 4.0f / 3.0f);
}

TEST_CASE("LayoutController appends active tracks in slot order",
          "[track_pipeline][layout_controller]") {
    TrackPipelineManager manager;
    auto first = std::make_unique<TrackPipeline>();
    first->file_id = 21;
    manager[1] = std::move(first);
    auto second = std::make_unique<TrackPipeline>();
    second->file_id = 42;
    manager[3] = std::move(second);

    LayoutController controller;
    LayoutState layout;
    controller.reset(layout);
    controller.append_tracks(layout, manager);

    REQUIRE(layout.order[0] == 1);
    REQUIRE(layout.order[1] == 3);
    REQUIRE(layout.order[2] == 0);
    REQUIRE(layout.order[3] == 0);

    const auto snapshot = controller.snapshot(layout);
    REQUIRE(snapshot.order[0] == 21);
    REQUIRE(snapshot.order[1] == 42);
    REQUIRE(snapshot.order[2] == -1);
    REQUIRE(snapshot.order[3] == -1);
}

TEST_CASE("TrackSnapshot builds track metadata",
          "[track_pipeline][track_snapshot]") {
    TrackPipelineManager manager;
    auto empty_metadata = std::make_unique<TrackPipeline>();
    empty_metadata->file_id = 9;
    empty_metadata->file_path = "synthetic.mp4";
    empty_metadata->video_width = 640;
    empty_metadata->video_height = 360;
    manager[2] = std::move(empty_metadata);

    auto infos = snapshot_track_infos(manager);
    REQUIRE(infos.size() == 1);
    REQUIRE(infos[0].file_id == 9);
    REQUIRE(infos[0].slot == 2);
    REQUIRE(infos[0].file_path == "synthetic.mp4");
    REQUIRE(infos[0].width == 640);
    REQUIRE(infos[0].height == 360);
    REQUIRE(infos[0].duration_us == 0);
    REQUIRE(infos[0].codec_name.empty());
    REQUIRE(infos[0].decoder_name.empty());

    TrackPipelineFactory factory;
    auto pipeline = factory.create_opened_pipeline(
        video_test_dir() + "/h264_9s_1920x1080.mp4",
        false);
    REQUIRE(pipeline);
    pipeline->file_id = 7;
    manager[0] = std::move(pipeline);

    infos = snapshot_track_infos(manager);
    REQUIRE(infos.size() == 2);
    REQUIRE(infos[0].file_id == 7);
    REQUIRE(infos[0].slot == 0);
    REQUIRE(infos[0].width == 1920);
    REQUIRE(infos[0].height == 1080);
    REQUIRE(infos[0].duration_us > 0);
    REQUIRE(infos[0].codec_name == "h264");
    REQUIRE_FALSE(infos[0].decoder_name.empty());
    REQUIRE(infos[1].file_id == 9);

    manager.clear();
}

TEST_CASE("TrackSnapshot builds render-loop diagnostics",
          "[track_pipeline][track_snapshot]") {
    TrackPipelineManager manager;
    REQUIRE(snapshot_render_loop_track_diagnostics(manager).empty());

    auto buffered = std::make_unique<TrackPipeline>();
    buffered->track_buffer = std::make_shared<TrackBuffer>(5, 1);
    TextureFrame frame;
    frame.pts_us = 1000;
    buffered->track_buffer->push_frame(frame);
    buffered->track_buffer->set_state(TrackState::Ready);
    manager[0] = std::move(buffered);

    manager[2] = std::make_unique<TrackPipeline>();

    auto diagnostics = snapshot_render_loop_track_diagnostics(manager);
    REQUIRE(diagnostics.size() == 2);
    REQUIRE(diagnostics[0].slot == 0);
    REQUIRE(diagnostics[0].buffer_count == 1);
    REQUIRE(diagnostics[0].buffer_capacity == 5);
    REQUIRE(diagnostics[0].buffer_state == TrackState::Ready);
    REQUIRE(diagnostics[1].slot == 2);
    REQUIRE(diagnostics[1].buffer_count == 0);
    REQUIRE(diagnostics[1].buffer_capacity == 0);
    REQUIRE(diagnostics[1].buffer_state == TrackState::Empty);
}

TEST_CASE("TrackSnapshot builds track perf stats",
          "[track_pipeline][track_snapshot]") {
    TrackPipeline track;
    track.file_id = 42;
    track.track_buffer = std::make_shared<TrackBuffer>(4, 1);
    TextureFrame buffered_frame;
    buffered_frame.pts_us = 111000;
    track.track_buffer->push_frame(buffered_frame);
    track.track_buffer->set_state(TrackState::Ready);

    TextureFrame current_frame;
    current_frame.pts_us = 1234567;
    current_frame.dts_us = 1200000;

    DecodePerfCounters::Snapshot decode_perf{
        120,
        480000,
        3500,
        0,
    };

    auto snapshot = snapshot_track_perf_stats(
        3, track, decode_perf, current_frame, 90, 1.5);

    REQUIRE(snapshot.frames_decoded == 120);
    REQUIRE(snapshot.stats.slot == 3);
    REQUIRE(snapshot.stats.file_id == 42);
    REQUIRE(snapshot.stats.buffer_count == 1);
    REQUIRE(snapshot.stats.buffer_capacity == 4);
    REQUIRE(snapshot.stats.buffer_state == TrackState::Ready);
    REQUIRE(snapshot.stats.current_pts_us == 1234567);
    REQUIRE(snapshot.stats.current_dts_us == 1200000);
    REQUIRE(snapshot.stats.avg_decode_ms == 4.0);
    REQUIRE(snapshot.stats.max_decode_ms == 3.5);
    REQUIRE(snapshot.stats.fps == 20.0);

    auto short_window = snapshot_track_perf_stats(
        3, track, decode_perf, std::nullopt, 90, 0.25);
    REQUIRE(short_window.stats.current_pts_us == 0);
    REQUIRE(short_window.stats.current_dts_us == kNoTimestampUs);
    REQUIRE(short_window.stats.fps == 0.0);
}

TEST_CASE("TrackSnapshot builds track GPU memory stats",
          "[track_pipeline][track_snapshot]") {
    TrackPipeline track;
    track.file_id = 64;
    track.track_buffer = std::make_shared<TrackBuffer>(3, 2);
    TextureFrame buffered_frame;
    buffered_frame.cpu_data = std::make_shared<std::vector<uint8_t>>(32);
    track.track_buffer->push_frame(buffered_frame);

    track.packet_queue = std::make_unique<PacketQueue>();
    AVPacket* packet = av_packet_alloc();
    REQUIRE(packet != nullptr);
    REQUIRE(av_new_packet(packet, 48) == 0);
    REQUIRE(track.packet_queue->try_push(packet));

    DecodeMemoryStats decode_stats;
    decode_stats.hardware_enabled = true;
    decode_stats.hardware_download_to_cpu = true;
    decode_stats.hw_format = 23;
    decode_stats.sw_format = 0;
    decode_stats.hw_width = 1920;
    decode_stats.hw_height = 1080;
    decode_stats.hw_initial_pool_size = 8;
    decode_stats.extra_hw_frames = 3;
    decode_stats.estimated_hw_frame_bytes = 1024;
    decode_stats.estimated_hw_pool_bytes = 8192;
    decode_stats.snapshot_pool.estimated_bytes = 512;
    decode_stats.exact_seek_candidate_cpu_bytes = 128;
    decode_stats.exact_seek_stable_cpu_bytes = 256;
    decode_stats.exact_seek_reorder_count = 5;
    decode_stats.exact_seek_pending_count = 2;
    decode_stats.exact_seek_stable_frame_count = 1;

    const auto buffer_bytes = track.track_buffer->estimated_cpu_bytes();
    const auto packet_bytes = track.packet_queue->estimated_bytes();
    auto stats = snapshot_track_gpu_memory_stats(2, track, &decode_stats, 4096);

    REQUIRE(stats.slot == 2);
    REQUIRE(stats.file_id == 64);
    REQUIRE(stats.buffer_count == 1);
    REQUIRE(stats.buffer_capacity == track.track_buffer->max_count());
    REQUIRE(stats.track_buffer_cpu_bytes == buffer_bytes);
    REQUIRE(stats.packet_queue_bytes == packet_bytes);
    REQUIRE(stats.hardware_enabled);
    REQUIRE(stats.hardware_download_to_cpu);
    REQUIRE(stats.hw_format == 23);
    REQUIRE(stats.hw_width == 1920);
    REQUIRE(stats.hw_height == 1080);
    REQUIRE(stats.hw_initial_pool_size == 8);
    REQUIRE(stats.extra_hw_frames == 3);
    REQUIRE(stats.decoder_frame_bytes == 1024);
    REQUIRE(stats.decoder_pool_bytes == 8192);
    REQUIRE(stats.exact_seek_snapshot_bytes == 512);
    REQUIRE(stats.presenter_copy_texture_bytes == 4096);
    REQUIRE(stats.exact_seek_candidate_cpu_bytes == 128);
    REQUIRE(stats.exact_seek_stable_cpu_bytes == 256);
    REQUIRE(stats.exact_seek_reorder_count == 5);
    REQUIRE(stats.exact_seek_pending_count == 2);
    REQUIRE(stats.exact_seek_stable_frame_count == 1);
    REQUIRE(stats.total_cpu_frame_bytes == buffer_bytes + 128 + 256);

    auto without_decode = snapshot_track_gpu_memory_stats(2, track, nullptr, 0);
    REQUIRE_FALSE(without_decode.hardware_enabled);
    REQUIRE(without_decode.decoder_pool_bytes == 0);
    REQUIRE(without_decode.total_cpu_frame_bytes == buffer_bytes);
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

TEST_CASE("TrackLifecycle resolves per-track seek target",
          "[track_pipeline][track_lifecycle]") {
    TrackPipeline no_demux_track;
    no_demux_track.offset_us = 250000;

    auto result = resolve_track_seek_target(no_demux_track, 1000000);
    REQUIRE(result.requested_target_us == 750000);
    REQUIRE(result.target_us == 750000);
    REQUIRE_FALSE(result.clamped);

    result = resolve_track_seek_target(no_demux_track, 100000);
    REQUIRE(result.requested_target_us == 0);
    REQUIRE(result.target_us == 0);
    REQUIRE_FALSE(result.clamped);

    TrackPipelineFactory factory;
    auto pipeline = factory.create_opened_pipeline(
        video_test_dir() + "/h264_9s_1920x1080.mp4",
        false);
    REQUIRE(pipeline);
    const int64_t track_end_us =
        track_pts_end_us_from_stats(pipeline->demux_thread->stats());
    REQUIRE(track_end_us > 0);

    result = resolve_track_seek_target(*pipeline, track_end_us + 1000000);
    REQUIRE(result.requested_target_us == track_end_us + 1000000);
    REQUIRE(result.target_us == track_end_us);
    REQUIRE(result.clamped);
}

TEST_CASE("TrackLifecycle applies track offset mutation",
          "[track_pipeline][track_lifecycle]") {
    TrackPipeline track;
    track.offset_us = 1000;

    std::vector<std::string> events;
    const TrackOffsetMutationHooks hooks{
        [&](size_t slot, int64_t offset_us) {
            events.push_back(
                std::to_string(slot) + ":" + std::to_string(offset_us));
        },
    };

    auto result = apply_track_offset_mutation(track, 2, 3000, hooks);
    REQUIRE(result.previous_offset_us == 1000);
    REQUIRE(result.offset_us == 3000);
    REQUIRE(result.changed);
    REQUIRE(track.offset_us == 3000);
    REQUIRE(events == std::vector<std::string>{"2:3000"});

    result = apply_track_offset_mutation(track, 2, 3000, hooks);
    REQUIRE(result.previous_offset_us == 3000);
    REQUIRE(result.offset_us == 3000);
    REQUIRE_FALSE(result.changed);
    REQUIRE(track.offset_us == 3000);
    REQUIRE(events == std::vector<std::string>{"2:3000", "2:3000"});
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

TEST_CASE("TrackLifecycle guards playback around track mutation",
          "[track_pipeline][track_lifecycle]") {
    std::vector<std::string> events;
    const TrackPlaybackMutationHooks hooks{
        [&]() { events.push_back("pause"); },
        [&]() { events.push_back("play"); },
        [&](bool playing) {
            events.push_back(std::string("playing:") + (playing ? "true" : "false"));
        },
    };

    const auto idle_state =
        pause_playback_for_track_mutation(false, hooks);
    REQUIRE_FALSE(idle_state.was_playing);
    REQUIRE(events.empty());
    rollback_track_mutation_playback(idle_state, hooks);
    finish_track_removal_playback(idle_state, true, hooks);
    REQUIRE(events.empty());

    const auto active_state =
        pause_playback_for_track_mutation(true, hooks);
    REQUIRE(active_state.was_playing);
    REQUIRE(events == std::vector<std::string>{"pause", "playing:false"});

    rollback_track_mutation_playback(active_state, hooks);
    REQUIRE(events == std::vector<std::string>{
        "pause",
        "playing:false",
        "play",
        "playing:true",
    });

    events.clear();
    const auto removal_without_tracks =
        pause_playback_for_track_mutation(true, hooks);
    finish_track_removal_playback(removal_without_tracks, false, hooks);
    REQUIRE(events == std::vector<std::string>{"pause", "playing:false"});

    events.clear();
    const auto removal_with_tracks =
        pause_playback_for_track_mutation(true, hooks);
    finish_track_removal_playback(removal_with_tracks, true, hooks);
    REQUIRE(events == std::vector<std::string>{
        "pause",
        "playing:false",
        "play",
        "playing:true",
    });
}

TEST_CASE("TrackLifecycle applies playback decode state in stable order",
          "[track_pipeline][track_lifecycle]") {
    TrackPipelineManager manager;
    auto first = std::make_unique<TrackPipeline>();
    first->file_id = 10;
    auto second = std::make_unique<TrackPipeline>();
    second->file_id = 12;
    manager[0] = std::move(first);
    manager[2] = std::move(second);

    std::vector<std::string> events;
    const TrackPlaybackDecodeStateHooks hooks{
        [&](size_t slot, TrackPipeline& track, bool enabled) {
            events.push_back("preroll:" + std::to_string(slot) + ":" +
                             std::to_string(track.file_id) + ":" +
                             (enabled ? "true" : "false"));
        },
        [&](size_t slot, TrackPipeline& track, bool paused) {
            events.push_back("decode:" + std::to_string(slot) + ":" +
                             std::to_string(track.file_id) + ":" +
                             (paused ? "true" : "false"));
        },
        [&](bool paused) {
            events.push_back(std::string("audio:") + (paused ? "true" : "false"));
        },
    };

    apply_track_playback_decode_state(manager, true, hooks);
    REQUIRE(events == std::vector<std::string>{
        "preroll:0:10:false",
        "preroll:2:12:false",
        "decode:0:10:false",
        "decode:2:12:false",
        "audio:false",
    });

    events.clear();
    apply_track_playback_decode_state(manager, false, hooks);
    REQUIRE(events == std::vector<std::string>{
        "preroll:0:10:true",
        "preroll:2:12:true",
        "decode:0:10:true",
        "decode:2:12:true",
        "audio:true",
    });

    events.clear();
    manager.clear();
    apply_track_playback_decode_state(manager, false, hooks);
    REQUIRE(events == std::vector<std::string>{"audio:true"});
}

TEST_CASE("TrackLifecycle applies decode pause fanout",
          "[track_pipeline][track_lifecycle]") {
    TrackPipelineManager manager;
    auto first = std::make_unique<TrackPipeline>();
    first->file_id = 20;
    auto second = std::make_unique<TrackPipeline>();
    second->file_id = 22;
    manager[1] = std::move(first);
    manager[3] = std::move(second);

    std::vector<std::string> events;
    const TrackDecodePauseHooks hooks{
        [&](size_t slot, TrackPipeline& track, bool paused) {
            events.push_back("decode:" + std::to_string(slot) + ":" +
                             std::to_string(track.file_id) + ":" +
                             (paused ? "true" : "false"));
        },
        [&](bool paused) {
            events.push_back(std::string("audio:") + (paused ? "true" : "false"));
        },
    };

    apply_track_decode_pause_state(manager, true, hooks);
    REQUIRE(events == std::vector<std::string>{
        "decode:1:20:true",
        "decode:3:22:true",
        "audio:true",
    });

    events.clear();
    manager.clear();
    apply_track_decode_pause_state(manager, false, hooks);
    REQUIRE(events == std::vector<std::string>{"audio:false"});
}

TEST_CASE("TrackStepPolicy applies video-only decode pause fanout",
          "[track_pipeline][track_step_policy]") {
    TrackPipelineManager manager;
    auto first = std::make_unique<TrackPipeline>();
    first->file_id = 30;
    auto second = std::make_unique<TrackPipeline>();
    second->file_id = 31;
    manager[0] = std::move(first);
    manager[2] = std::move(second);

    std::vector<std::string> events;
    apply_track_video_decode_pause_state(
        manager,
        false,
        [&](size_t slot, TrackPipeline& track, bool paused) {
            events.push_back("video:" + std::to_string(slot) + ":" +
                             std::to_string(track.file_id) + ":" +
                             (paused ? "true" : "false"));
        });

    REQUIRE(events == std::vector<std::string>{
        "video:0:30:false",
        "video:2:31:false",
    });
}

TEST_CASE("TrackStepPolicy detects buffering tracks",
          "[track_pipeline][track_step_policy]") {
    TrackPipelineManager manager;
    REQUIRE_FALSE(has_buffering_track(manager));

    auto ready = std::make_unique<TrackPipeline>();
    ready->track_buffer = std::make_shared<TrackBuffer>();
    ready->track_buffer->set_state(TrackState::Ready);
    manager[0] = std::move(ready);
    REQUIRE_FALSE(has_buffering_track(manager));

    auto buffering = std::make_unique<TrackPipeline>();
    buffering->track_buffer = std::make_shared<TrackBuffer>();
    buffering->track_buffer->set_state(TrackState::Buffering);
    manager[2] = std::move(buffering);
    REQUIRE(has_buffering_track(manager));

    manager[2]->track_buffer->set_state(TrackState::Flushing);
    REQUIRE_FALSE(has_buffering_track(manager));
}

TEST_CASE("TrackStepPolicy retreats tracks only when all can retreat",
          "[track_pipeline][track_step_policy]") {
    TrackPipelineManager empty_manager;
    REQUIRE(retreat_tracks_if_all_can_retreat(empty_manager));

    const auto make_retreatable_track = [](int64_t previous_pts,
                                           int64_t current_pts) {
        auto track = std::make_unique<TrackPipeline>();
        track->track_buffer = std::make_shared<TrackBuffer>();
        TextureFrame previous;
        previous.pts_us = previous_pts;
        TextureFrame current;
        current.pts_us = current_pts;
        track->track_buffer->push_frame(previous);
        track->track_buffer->push_frame(current);
        REQUIRE(track->track_buffer->advance());
        REQUIRE(track->track_buffer->can_retreat());
        REQUIRE(track->track_buffer->peek(0)->pts_us == current_pts);
        return track;
    };

    TrackPipelineManager success_manager;
    success_manager[0] = make_retreatable_track(100, 200);
    success_manager[2] = make_retreatable_track(300, 400);

    REQUIRE(retreat_tracks_if_all_can_retreat(success_manager));
    REQUIRE(success_manager[0]->track_buffer->peek(0)->pts_us == 100);
    REQUIRE(success_manager[2]->track_buffer->peek(0)->pts_us == 300);

    TrackPipelineManager failure_manager;
    failure_manager[0] = make_retreatable_track(500, 600);
    auto non_retreatable = std::make_unique<TrackPipeline>();
    non_retreatable->track_buffer = std::make_shared<TrackBuffer>();
    TextureFrame only_frame;
    only_frame.pts_us = 700;
    non_retreatable->track_buffer->push_frame(only_frame);
    REQUIRE_FALSE(non_retreatable->track_buffer->can_retreat());
    failure_manager[1] = std::move(non_retreatable);

    REQUIRE_FALSE(retreat_tracks_if_all_can_retreat(failure_manager));
    REQUIRE(failure_manager[0]->track_buffer->peek(0)->pts_us == 600);
    REQUIRE(failure_manager[1]->track_buffer->peek(0)->pts_us == 700);
}

TEST_CASE("TrackStepPolicy computes minimum current frame duration",
          "[track_pipeline][track_step_policy]") {
    TrackPipelineManager empty_manager;
    REQUIRE(compute_min_current_frame_duration_us(empty_manager) == 33333);

    const auto make_track_with_current_duration = [](int64_t duration_us) {
        auto track = std::make_unique<TrackPipeline>();
        track->track_buffer = std::make_shared<TrackBuffer>();
        TextureFrame frame;
        frame.pts_us = 0;
        frame.duration_us = duration_us;
        track->track_buffer->push_frame(frame);
        return track;
    };

    TrackPipelineManager manager;
    manager[0] = make_track_with_current_duration(33333);
    manager[2] = make_track_with_current_duration(16667);
    REQUIRE(compute_min_current_frame_duration_us(manager) == 16667);

    TrackPipelineManager ignored_manager;
    ignored_manager[0] = make_track_with_current_duration(0);
    ignored_manager[1] = make_track_with_current_duration(-1);
    REQUIRE(compute_min_current_frame_duration_us(ignored_manager) == 33333);

    TrackPipelineManager oversized_manager;
    oversized_manager[0] = make_track_with_current_duration(100001);
    REQUIRE(compute_min_current_frame_duration_us(oversized_manager) == 33333);
}

TEST_CASE("TrackPrerollPolicy detects preroll-blocking tracks",
          "[track_pipeline][track_preroll_policy]") {
    TrackPipelineManager manager;
    REQUIRE_FALSE(has_preroll_blocking_track(manager));

    auto ready = std::make_unique<TrackPipeline>();
    ready->track_buffer = std::make_shared<TrackBuffer>();
    ready->track_buffer->set_state(TrackState::Ready);
    manager[0] = std::move(ready);
    REQUIRE_FALSE(has_preroll_blocking_track(manager));

    manager[0]->track_buffer->set_state(TrackState::Buffering);
    REQUIRE(has_preroll_blocking_track(manager));

    manager[0]->track_buffer->set_state(TrackState::Empty);
    REQUIRE(has_preroll_blocking_track(manager));

    manager[0]->track_buffer->set_state(TrackState::Flushing);
    REQUIRE(has_preroll_blocking_track(manager));

    manager[0]->track_buffer->set_state(TrackState::Error);
    REQUIRE_FALSE(has_preroll_blocking_track(manager));

    auto missing_buffer = std::make_unique<TrackPipeline>();
    manager[1] = std::move(missing_buffer);
    REQUIRE(has_preroll_blocking_track(manager));
}

TEST_CASE("TrackPreviewPolicy builds paused preview snapshots",
          "[track_pipeline][track_preview_policy]") {
    const auto make_track =
        [](TrackState state, std::optional<int64_t> pts_us) {
            auto track = std::make_unique<TrackPipeline>();
            track->track_buffer = std::make_shared<TrackBuffer>();
            if (pts_us.has_value()) {
                TextureFrame frame;
                frame.pts_us = *pts_us;
                track->track_buffer->push_frame(frame);
            }
            track->track_buffer->set_state(state);
            return track;
        };

    TrackPipelineManager empty_manager;
    auto empty = build_paused_preview_snapshot(empty_manager);
    REQUIRE_FALSE(empty.ready_to_present);
    REQUIRE_FALSE(empty.decision.should_present);

    TrackPipelineManager ready_manager;
    ready_manager[0] = make_track(TrackState::Ready, 1000);
    ready_manager[2] = make_track(TrackState::Ready, 2000);
    auto ready = build_paused_preview_snapshot(ready_manager);
    REQUIRE(ready.ready_to_present);
    REQUIRE(ready.decision.should_present);
    REQUIRE(ready.decision.frames[0]->pts_us == 1000);
    REQUIRE_FALSE(ready.decision.frames[1].has_value());
    REQUIRE(ready.decision.frames[2]->pts_us == 2000);

    ready_manager[1] = make_track(TrackState::Ready, std::nullopt);
    auto eof_ready = build_paused_preview_snapshot(ready_manager);
    REQUIRE(eof_ready.ready_to_present);
    REQUIRE_FALSE(eof_ready.decision.frames[1].has_value());

    ready_manager[3] = make_track(TrackState::Buffering, std::nullopt);
    auto buffering_empty = build_paused_preview_snapshot(ready_manager);
    REQUIRE_FALSE(buffering_empty.ready_to_present);
    REQUIRE_FALSE(buffering_empty.decision.should_present);

    ready_manager[3] = make_track(TrackState::Buffering, 3000);
    auto buffering_with_frame = build_paused_preview_snapshot(ready_manager);
    REQUIRE_FALSE(buffering_with_frame.ready_to_present);
    REQUIRE_FALSE(buffering_with_frame.decision.should_present);

    TrackPipelineManager missing_buffer_manager;
    missing_buffer_manager[0] = std::make_unique<TrackPipeline>();
    auto missing_buffer =
        build_paused_preview_snapshot(missing_buffer_manager);
    REQUIRE_FALSE(missing_buffer.ready_to_present);
    REQUIRE_FALSE(missing_buffer.decision.should_present);
}

TEST_CASE("TrackPreviewPolicy builds available paused frame snapshots",
          "[track_pipeline][track_preview_policy]") {
    const auto make_track =
        [](TrackState state, std::optional<int64_t> pts_us) {
            auto track = std::make_unique<TrackPipeline>();
            track->track_buffer = std::make_shared<TrackBuffer>();
            if (pts_us.has_value()) {
                TextureFrame frame;
                frame.pts_us = *pts_us;
                track->track_buffer->push_frame(frame);
            }
            track->track_buffer->set_state(state);
            return track;
        };

    TrackPipelineManager empty_manager;
    auto empty = build_available_paused_frame_snapshot(empty_manager);
    REQUIRE_FALSE(empty.has_frame);
    REQUIRE_FALSE(empty.decision.should_present);

    TrackPipelineManager manager;
    manager[0] = make_track(TrackState::Ready, 1000);
    manager[1] = make_track(TrackState::Ready, std::nullopt);
    manager[2] = std::make_unique<TrackPipeline>();
    manager[3] = make_track(TrackState::Buffering, 3000);

    auto snapshot = build_available_paused_frame_snapshot(manager);
    REQUIRE(snapshot.has_frame);
    REQUIRE_FALSE(snapshot.decision.should_present);
    REQUIRE(snapshot.decision.frames[0]->pts_us == 1000);
    REQUIRE_FALSE(snapshot.decision.frames[1].has_value());
    REQUIRE_FALSE(snapshot.decision.frames[2].has_value());
    REQUIRE(snapshot.decision.frames[3]->pts_us == 3000);
}

TEST_CASE("TrackPresentPolicy carries forward active last frames",
          "[track_pipeline][track_present_policy]") {
    const auto make_track = [](int64_t offset_us) {
        auto track = std::make_unique<TrackPipeline>();
        track->offset_us = offset_us;
        return track;
    };
    const auto make_frame = [](int64_t pts_us) {
        TextureFrame frame;
        frame.pts_us = pts_us;
        return frame;
    };

    TrackPipelineManager manager;
    manager[0] = make_track(0);
    manager[1] = make_track(3000);
    manager[2] = make_track(-1000);

    PresentDecision last_decision;
    last_decision.frames[0] = make_frame(100);
    last_decision.frames[1] = make_frame(200);
    last_decision.frames[2] = make_frame(300);
    last_decision.frames[3] = make_frame(400);

    PresentDecision decision;
    decision.current_pts_us = 2000;
    decision.should_present = true;
    decision.frames[2] = make_frame(999);

    apply_present_carry_forward(manager, last_decision, decision);

    REQUIRE(decision.should_present);
    REQUIRE(decision.frames[0]->pts_us == 100);
    REQUIRE_FALSE(decision.frames[1].has_value());
    REQUIRE(decision.frames[2]->pts_us == 999);
    REQUIRE_FALSE(decision.frames[3].has_value());
}

TEST_CASE("TrackPresentPolicy computes empty-buffer EOF clamp facts",
          "[track_pipeline][track_present_policy]") {
    const auto make_frame = [](int64_t pts_us, int64_t duration_us) {
        TextureFrame frame;
        frame.pts_us = pts_us;
        frame.duration_us = duration_us;
        return frame;
    };
    const auto make_track =
        [&](int64_t offset_us, std::optional<TextureFrame> queued_frame) {
            auto track = std::make_unique<TrackPipeline>();
            track->offset_us = offset_us;
            track->track_buffer = std::make_shared<TrackBuffer>();
            if (queued_frame.has_value()) {
                track->track_buffer->push_frame(*queued_frame);
            }
            return track;
        };

    TrackPipelineManager empty_manager;
    auto empty = compute_empty_buffer_eof_clamp(empty_manager, PresentDecision());
    REQUIRE(empty.all_active_buffers_empty);
    REQUIRE(empty.max_end_pts_us == 0);

    TrackPipelineManager manager;
    manager[0] = make_track(100, std::nullopt);
    manager[1] = make_track(-20, std::nullopt);
    PresentDecision last_decision;
    last_decision.frames[0] = make_frame(1000, 40);
    last_decision.frames[1] = make_frame(2000, 50);

    auto clamp = compute_empty_buffer_eof_clamp(manager, last_decision);
    REQUIRE(clamp.all_active_buffers_empty);
    REQUIRE(clamp.max_end_pts_us == 2030);

    manager[2] = make_track(0, make_frame(3000, 30));
    auto non_empty = compute_empty_buffer_eof_clamp(manager, last_decision);
    REQUIRE_FALSE(non_empty.all_active_buffers_empty);

    TrackPipelineManager missing_buffer_manager;
    missing_buffer_manager[0] = std::make_unique<TrackPipeline>();
    missing_buffer_manager[0]->offset_us = 7;
    PresentDecision missing_last;
    missing_last.frames[0] = make_frame(10, 5);
    auto missing_buffer =
        compute_empty_buffer_eof_clamp(missing_buffer_manager, missing_last);
    REQUIRE(missing_buffer.all_active_buffers_empty);
    REQUIRE(missing_buffer.max_end_pts_us == 22);
}

TEST_CASE("TrackPresentPolicy computes next frame event PTS",
          "[track_pipeline][track_present_policy]") {
    const auto make_track =
        [](std::optional<TextureFrame> queued_frame) {
            auto track = std::make_unique<TrackPipeline>();
            track->track_buffer = std::make_shared<TrackBuffer>();
            if (queued_frame.has_value()) {
                track->track_buffer->push_frame(*queued_frame);
            }
            return track;
        };
    const auto make_frame = [](int64_t pts_us, int64_t duration_us) {
        TextureFrame frame;
        frame.pts_us = pts_us;
        frame.duration_us = duration_us;
        return frame;
    };

    TrackPipelineManager empty_manager;
    REQUIRE_FALSE(compute_next_frame_event_pts_us(
        empty_manager,
        100).has_value());

    TrackPipelineManager manager;
    manager[0] = make_track(make_frame(140, 10));
    manager[1] = make_track(make_frame(80, 30));
    manager[2] = make_track(std::nullopt);
    manager[3] = std::make_unique<TrackPipeline>();

    auto next_event = compute_next_frame_event_pts_us(manager, 100);
    REQUIRE(next_event.has_value());
    REQUIRE(*next_event == 110);

    manager[2] = make_track(make_frame(105, 20));
    next_event = compute_next_frame_event_pts_us(manager, 100);
    REQUIRE(next_event.has_value());
    REQUIRE(*next_event == 105);
}

TEST_CASE("TrackStepPolicy builds step-forward decisions",
          "[track_pipeline][track_step_policy]") {
    TrackPipelineManager empty_manager;
    PresentDecision decision;
    PresentDecision last_decision;
    REQUIRE_FALSE(build_step_forward_decision(
        empty_manager,
        1000,
        1000,
        last_decision,
        decision));
    REQUIRE_FALSE(decision.should_present);

    const auto make_track_with_frames =
        [](std::initializer_list<int64_t> pts_values) {
            auto track = std::make_unique<TrackPipeline>();
            track->track_buffer = std::make_shared<TrackBuffer>();
            for (const int64_t pts : pts_values) {
                TextureFrame frame;
                frame.pts_us = pts;
                track->track_buffer->push_frame(frame);
            }
            return track;
        };

    TrackPipelineManager manager;
    manager[1] = make_track_with_frames({1000, 2000, 3000});

    REQUIRE(build_step_forward_decision(
        manager,
        1000,
        1000,
        last_decision,
        decision));
    REQUIRE(decision.should_present);
    REQUIRE(decision.current_pts_us == 1000);
    REQUIRE(decision.frames[1].has_value());
    REQUIRE(decision.frames[1]->pts_us == 2000);

    TextureFrame last_frame;
    last_frame.pts_us = 2000;
    last_decision.frames[1] = last_frame;
    REQUIRE(build_step_forward_decision(
        manager,
        1000,
        1000,
        last_decision,
        decision));
    REQUIRE(decision.frames[1]->pts_us == 3000);

    TrackPipelineManager gap_manager;
    gap_manager[0] = make_track_with_frames({1000, 10000});
    PresentDecision gap_decision;
    REQUIRE_FALSE(build_step_forward_decision(
        gap_manager,
        1000,
        1000,
        PresentDecision(),
        gap_decision));
    REQUIRE_FALSE(gap_decision.should_present);
}

TEST_CASE("TrackStepPolicy discards consumed step-forward frames",
          "[track_pipeline][track_step_policy]") {
    const auto make_track_with_frames =
        [](std::initializer_list<int64_t> pts_values, int64_t offset_us = 0) {
            auto track = std::make_unique<TrackPipeline>();
            track->offset_us = offset_us;
            track->track_buffer = std::make_shared<TrackBuffer>();
            for (const int64_t pts : pts_values) {
                TextureFrame frame;
                frame.pts_us = pts;
                track->track_buffer->push_frame(frame);
            }
            return track;
        };

    TrackPipelineManager manager;
    manager[0] = make_track_with_frames({100, 200, 300});
    manager[1] = make_track_with_frames({100, 200, 300});
    manager[2] = make_track_with_frames({100, 200, 300}, 50);

    PresentDecision decision;
    TextureFrame selected;
    selected.pts_us = 200;
    decision.frames[0] = selected;

    PresentDecision last_decision;
    TextureFrame last_selected;
    last_selected.pts_us = 200;
    last_decision.frames[1] = last_selected;

    discard_step_forward_consumed_frames(
        manager,
        250,
        decision,
        last_decision);

    REQUIRE(manager[0]->track_buffer->peek(0)->pts_us == 300);
    REQUIRE(manager[1]->track_buffer->peek(0)->pts_us == 300);
    REQUIRE(manager[2]->track_buffer->peek(0)->pts_us == 300);
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

TEST_CASE("TrackLifecycle binds existing tracks to render sink",
          "[track_pipeline][track_lifecycle]") {
    TrackPipelineManager manager;
    auto track = std::make_unique<TrackPipeline>();
    track->track_buffer = std::make_shared<TrackBuffer>(2, 0);
    TextureFrame frame;
    frame.pts_us = 1000;
    frame.duration_us = 100;
    track->track_buffer->push_frame(frame);
    manager[0] = std::move(track);
    manager[2] = std::make_unique<TrackPipeline>();

    Clock clock([] { return int64_t{0}; });
    clock.seek(1000);
    RenderSink render_sink(clock);
    bind_existing_tracks_to_render_sink(manager, render_sink);

    const auto decision = render_sink.evaluate();
    REQUIRE(decision.should_present);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1000);
    REQUIRE_FALSE(decision.frames[1].has_value());
    REQUIRE_FALSE(decision.frames[2].has_value());
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
