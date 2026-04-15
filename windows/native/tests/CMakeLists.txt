# ==================== Video Renderer Tests ====================

add_executable(video_renderer_tests
    test_utils.cpp
    test_clock.cpp
    test_packet_queue.cpp
    test_bidi_ring_buffer.cpp
    test_track_buffer.cpp
    test_demux_thread.cpp
    test_frame_converter.cpp
    test_decode_thread.cpp
    test_d3d11_device.cpp
    test_d3d11_texture.cpp
    test_d3d11_shader.cpp
    test_render_sink.cpp
    test_renderer_integration.cpp
)

set_target_properties(video_renderer_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

if(TARGET copy_ffmpeg_dlls)
    add_dependencies(video_renderer_tests copy_ffmpeg_dlls)
endif()

target_link_libraries(video_renderer_tests PRIVATE
    video_renderer_lib
    Catch2::Catch2WithMain
    spdlog::spdlog_header_only
)

target_compile_definitions(video_renderer_tests PRIVATE
    VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
)

add_test(NAME video_renderer_tests COMMAND video_renderer_tests)

# ==================== Analysis Tests ====================

add_executable(analysis_tests
    test_analysis_parsers.cpp
    test_analysis_generator.cpp
)

set_target_properties(analysis_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

if(TARGET copy_ffmpeg_dlls)
    add_dependencies(analysis_tests copy_ffmpeg_dlls)
endif()

target_link_libraries(analysis_tests PRIVATE
    analysis_lib
    Catch2::Catch2WithMain
    spdlog::spdlog_header_only
)

target_compile_definitions(analysis_tests PRIVATE
    VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
)

add_test(NAME analysis_tests COMMAND analysis_tests)

# ==================== C FFI Validation ====================

add_executable(test_ffi_c
    test_ffi_c.c
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

add_test(NAME test_ffi_c COMMAND test_ffi_c)
