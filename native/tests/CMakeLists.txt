# ==================== Video Renderer Tests ====================

add_executable(video_renderer_tests
    test_main.cpp
    renderer/test_utils.cpp
    renderer/test_clock.cpp
    renderer/test_packet_queue.cpp
    renderer/test_bidi_ring_buffer.cpp
    renderer/test_track_buffer.cpp
    renderer/test_demux_thread.cpp
    renderer/test_frame_converter.cpp
    renderer/test_decode_thread.cpp
    renderer/test_d3d11_device.cpp
    renderer/test_d3d11_texture.cpp
    renderer/test_d3d11_shader.cpp
    renderer/test_render_sink.cpp
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
set_tests_properties(video_renderer_tests PROPERTIES TIMEOUT 180)

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
    VTM_DECODER_PATH="${VTM_DECODER_PATH}"
)

add_test(NAME analysis_tests COMMAND analysis_tests)
set_tests_properties(analysis_tests PROPERTIES TIMEOUT 180)

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

add_test(NAME test_ffi_c COMMAND test_ffi_c)
set_tests_properties(test_ffi_c PROPERTIES TIMEOUT 30)
