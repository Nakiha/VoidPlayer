cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "video_renderer_plugin.cpp"
  "analysis_ffi.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app flutter_wrapper_plugin)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib")
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# ==================== Native Video Renderer ====================

if(FFMPEG_FOUND)

# Video renderer sources
set(VR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/renderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/logging.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/clock.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/d3d11/device.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/d3d11/texture.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/d3d11/shader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/decode/demux_thread.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/decode/decode_thread.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/decode/frame_converter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/decode/hw/hw_decode_provider.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/decode/hw/d3d11va_provider.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/buffer/packet_queue.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/buffer/bidi_ring_buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/buffer/track_buffer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/sync/render_sink.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/sync/seek_controller.cpp"
)

# Analysis library sources
set(ANALYSIS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/analysis/parsers/vbt_parser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/analysis/parsers/vbi_parser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/analysis/parsers/vbs2_parser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/analysis/analysis_manager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/analysis/generators/analysis_generator.cpp"
)

target_sources(${BINARY_NAME} PRIVATE ${VR_SOURCES} ${ANALYSIS_SOURCES})

target_include_directories(${BINARY_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../native"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${FFMPEG_INCLUDE_DIR}"
)

target_link_libraries(${BINARY_NAME} PRIVATE
    spdlog::spdlog_header_only
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${SWSCALE_LIBRARY}
    dxgi
    d3d11
    d3dcompiler
)

target_compile_definitions(${BINARY_NAME} PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# Embed shader source into binary (no runtime file dependency)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/shaders/multitrack.hlsl" MULTITRACK_HLSL)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../native/video_renderer/shaders/embedded_shaders.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.h"
    @ONLY
)

# Copy FFmpeg DLLs to build output
set(FFMPEG_DLL_DIR "${FFMPEG_ROOT}/bin")
if(EXISTS "${FFMPEG_DLL_DIR}")
    file(GLOB FFMPEG_DLL_FILES "${FFMPEG_DLL_DIR}/*.dll")
    foreach(DLL ${FFMPEG_DLL_FILES})
        add_custom_command(TARGET ${BINARY_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "$<TARGET_FILE_DIR:${BINARY_NAME}>"
        )
    endforeach()
endif()

endif(FFMPEG_FOUND)

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)
