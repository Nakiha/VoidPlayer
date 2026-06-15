# ==================== Video Renderer Tests ====================

add_executable(video_renderer_tests
    test_main.cpp
    ../renderer/exports/ffi_marshalling.cpp
    ../renderer/exports/ffi_player_commands.cpp
    ../renderer/exports/ffi_player_lifecycle.cpp
    ../renderer/exports/ffi_process_globals.cpp
    ../renderer/exports/ffi_player_registry.cpp
    ../../windows/runner/native_player_registry.cpp
    renderer/test_utils.cpp
    renderer/test_clock.cpp
    renderer/test_logging.cpp
    renderer/test_ffi_marshalling.cpp
    renderer/test_ffi_player_commands.cpp
    renderer/test_ffi_player_lifecycle.cpp
    renderer/test_ffi_process_globals.cpp
    renderer/test_native_diagnostics_session.cpp
    renderer/test_windows_crash_handler.cpp
    renderer/test_win_utf8.cpp
    renderer/test_native_player.cpp
    renderer/test_renderer_playback_command_policy.cpp
    renderer/test_renderer_seek_log_policy.cpp
    renderer/test_device_loss_policy.cpp
    renderer/test_renderer_lifecycle.cpp
    renderer/test_renderer_config_validation.cpp
    renderer/test_renderer_stress.cpp
    renderer/test_packet_queue.cpp
    renderer/test_audio_mixer.cpp
    renderer/test_audio_track_registry.cpp
    renderer/test_pcm_buffer.cpp
    renderer/test_seek_coordinator.cpp
    renderer/test_bidi_ring_buffer.cpp
    renderer/test_track_buffer.cpp
    renderer/test_demux_thread.cpp
    renderer/test_av_frame_lifetime.cpp
    renderer/test_codec_loop.cpp
    renderer/test_decode_drain_policy.cpp
    renderer/test_decode_exact_seek_reorder.cpp
    renderer/test_decode_frame_drainer.cpp
    renderer/test_decode_frame_receive_loop.cpp
    renderer/test_decode_packet_sender.cpp
    renderer/test_decode_preroll_policy.cpp
    renderer/test_decode_seek_epoch.cpp
    renderer/test_decoded_frame_publisher.cpp
    renderer/test_decode_loop_policy.cpp
    renderer/test_exact_seek_candidate_store.cpp
    renderer/test_exact_seek_frame_publisher.cpp
    renderer/test_exact_seek_publish_policy.cpp
    renderer/test_exact_seek_window.cpp
    renderer/test_frame_timestamp_rescaler.cpp
    renderer/test_frame_converter.cpp
    renderer/test_decode_thread.cpp
    renderer/test_d3d11_device.cpp
    renderer/test_d3d11_texture.cpp
    renderer/test_d3d11_shader.cpp
    renderer/test_windows_display_resolver.cpp
    renderer/test_windows_presentation_policy.cpp
    renderer/test_windows_dcomp_composite.cpp
    renderer/test_shared_fp16_ring.cpp
    renderer/test_shared_source_cache_ring.cpp
    renderer/test_render_sink.cpp
    renderer/test_track_buffer_budget.cpp
    renderer/test_track_pipeline.cpp
    renderer/test_renderer_integration.cpp
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

if(BUILD_ANALYSIS AND BUILD_ANALYSIS_TESTS)
    # ==================== Analysis Tests ====================

    add_executable(analysis_tests
        test_main.cpp
        analysis/test_analysis_parsers.cpp
        analysis/test_analysis_generator.cpp
        analysis/test_analysis_data.cpp
        analysis/test_analysis_ffi_handles.cpp
        analysis/test_analysis_ffi_stubs.cpp
        ../../windows/runner/analysis_ffi.cpp
    )
    void_apply_native_compile_options(analysis_tests)

    set_target_properties(analysis_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )

    if(TARGET copy_ffmpeg_dlls)
        add_dependencies(analysis_tests copy_ffmpeg_dlls)
    endif()

    target_link_libraries(analysis_tests PRIVATE
        analysis_lib
        Catch2::Catch2
        spdlog::spdlog_header_only
    )

    target_include_directories(analysis_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/analysis"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../windows/runner"
    )

    target_compile_definitions(analysis_tests PRIVATE
        VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
        FFMPEG_ANALYZER_PATH="${FFMPEG_ANALYZER_PATH}"
        VOIDPLAYER_ANALYSIS_FFI_TESTING
    )

    add_test(NAME analysis_tests COMMAND analysis_tests)
    set_tests_properties(analysis_tests PROPERTIES
        TIMEOUT 180
        LABELS "windows;analysis;contract"
    )
endif()

if(BUILD_FFI)
    # ==================== C FFI Validation ====================

    add_executable(test_ffi_c
        ffi/test_ffi_c.c
    )

    set_target_properties(test_ffi_c PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )

    target_include_directories(test_ffi_c PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
    )

    target_link_libraries(test_ffi_c PRIVATE
        video_renderer_ffi
        spdlog::spdlog_header_only
    )

    if(TARGET copy_ffmpeg_dlls)
        add_dependencies(test_ffi_c copy_ffmpeg_dlls)
    endif()

    add_test(NAME test_ffi_c
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:test_ffi_c>$<SEMICOLON>$ENV{PATH}"
            "$<TARGET_FILE:test_ffi_c>"
    )
    set_tests_properties(test_ffi_c PROPERTIES
        TIMEOUT 30
        LABELS "windows;ffi;canary"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:test_ffi_c>"
    )
endif()
