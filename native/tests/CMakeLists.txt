# ==================== Video Renderer Tests ====================

add_executable(video_renderer_tests
    test_main.cpp
    support/test_utils.cpp
    common/test_clock.cpp
    common/test_logging.cpp
    windows/test_win_utf8.cpp
    windows/test_d3d11va_provider.cpp
    windows/test_windows_d3d11_target_ring.cpp
    windows/test_windows_d3d11_viewport_renderer.cpp
    windows/test_windows_d3d11_presentation_backend.cpp
    windows/test_windows_presentation_policy.cpp
    renderer/test_renderer_playback_command_policy.cpp
    renderer/test_renderer_presentation_completion.cpp
    renderer/test_renderer_seek_log_policy.cpp
    renderer/test_renderer_lifecycle.cpp
    renderer/test_renderer_config_validation.cpp
    media/test_packet_queue.cpp
    audio/test_audio_mixer.cpp
    audio/test_audio_track_registry.cpp
    audio/test_pcm_buffer.cpp
    renderer/test_seek_coordinator.cpp
    renderer/test_bidi_ring_buffer.cpp
    renderer/test_track_buffer.cpp
    media/test_demux_thread.cpp
    decode/test_av_frame_lifetime.cpp
    decode/test_codec_loop.cpp
    decode/test_decode_drain_policy.cpp
    decode/test_decode_exact_seek_reorder.cpp
    decode/test_decode_frame_drainer.cpp
    decode/test_decode_frame_receive_loop.cpp
    decode/test_decode_packet_sender.cpp
    decode/test_decode_preroll_policy.cpp
    decode/test_decode_seek_epoch.cpp
    decode/test_decoded_frame_publisher.cpp
    decode/test_decode_loop_policy.cpp
    decode/test_exact_seek_candidate_store.cpp
    decode/test_exact_seek_frame_publisher.cpp
    decode/test_exact_seek_publish_policy.cpp
    decode/test_exact_seek_window.cpp
    decode/test_frame_timestamp_rescaler.cpp
    decode/test_frame_converter.cpp
    decode/test_decode_thread.cpp
    renderer/test_render_sink.cpp
    renderer/test_track_buffer_budget.cpp
    renderer/test_track_pipeline.cpp
)

set_target_properties(video_renderer_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

if(TARGET copy_ffmpeg_dlls)
    add_dependencies(video_renderer_tests copy_ffmpeg_dlls)
endif()

target_link_libraries(video_renderer_tests PRIVATE
    video_renderer_lib
    Catch2::Catch2
    spdlog::spdlog_header_only
)

target_include_directories(video_renderer_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/renderer"
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
)

target_compile_definitions(video_renderer_tests PRIVATE
    VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
    FFMPEG_EXE_PATH="${FFMPEG_ROOT}/bin/ffmpeg.exe"
)
void_apply_native_compile_options(video_renderer_tests)

add_test(NAME video_renderer_tests COMMAND video_renderer_tests)
set_tests_properties(video_renderer_tests PROPERTIES
    TIMEOUT 180
    LABELS "windows;native;contract;integration"
)
