add_library(video_renderer_lib STATIC ${VOID_RENDERER_SOURCES})
void_apply_native_compile_options(video_renderer_lib)

target_include_directories(video_renderer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_include_directories(video_renderer_lib SYSTEM PUBLIC
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(video_renderer_lib PUBLIC
    spdlog::spdlog_header_only
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${SWSCALE_LIBRARY}
    dxgi
    d3d11
    d3dcompiler
    winmm
)

void_configure_renderer_shaders("${CMAKE_CURRENT_BINARY_DIR}")

if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
