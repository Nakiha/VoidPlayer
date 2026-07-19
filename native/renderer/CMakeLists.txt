add_library(video_renderer_core STATIC ${VOID_RENDERER_CORE_SOURCES})
void_apply_native_compile_options(video_renderer_core)

target_include_directories(video_renderer_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_include_directories(video_renderer_core SYSTEM PUBLIC
    ${FFMPEG_INCLUDE_DIR}
    ${VOID_MINIAUDIO_INCLUDE_DIR}
)

target_link_libraries(video_renderer_core PUBLIC
    void_video_decode_core
    spdlog::spdlog_header_only
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
)
target_compile_definitions(video_renderer_core PUBLIC
    VOID_BUILD_ANALYSIS=$<BOOL:${BUILD_ANALYSIS}>)

add_library(video_renderer_lib STATIC
    ${VOID_RENDERER_WINDOWS_SOURCES}
)
void_apply_native_compile_options(video_renderer_lib)

target_include_directories(video_renderer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_include_directories(video_renderer_lib SYSTEM PUBLIC
    ${FFMPEG_INCLUDE_DIR}
    ${VOID_MINIAUDIO_INCLUDE_DIR}
)

target_link_libraries(video_renderer_lib PUBLIC
    video_renderer_core
    spdlog::spdlog_header_only
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    d3d11
    d3dcompiler
    dxgi
    winmm
)
if(BUILD_ANALYSIS)
    target_link_libraries(video_renderer_lib PUBLIC analysis_lib)
endif()

target_compile_definitions(video_renderer_lib PUBLIC
    VOID_BUILD_ANALYSIS=$<BOOL:${BUILD_ANALYSIS}>)
