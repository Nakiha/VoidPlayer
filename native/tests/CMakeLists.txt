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
    test_seek_controller.cpp
    test_render_sink.cpp
    test_renderer_integration.cpp
)

# Output test exe to same dir as pybind11 module so FFmpeg DLLs are shared
set_target_properties(video_renderer_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

# Ensure FFmpeg DLLs are copied before test discovery runs
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

enable_testing()
add_test(NAME video_renderer_tests COMMAND video_renderer_tests)
