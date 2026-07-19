if(NOT TARGET Threads::Threads)
    find_package(Threads REQUIRED)
endif()

add_library(quality_metrics_lib STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/quality/quality_metrics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/quality/quality_metrics_cpu.cpp"
)
void_apply_native_compile_options(quality_metrics_lib)
target_include_directories(quality_metrics_lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _VOID_SYSTEM_PROCESSOR)
if(_VOID_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|x64|i[3-6]86)$")
    add_library(quality_metrics_avx2 OBJECT
        "${CMAKE_CURRENT_SOURCE_DIR}/quality/quality_metrics_avx2.cpp")
    void_apply_native_compile_options(quality_metrics_avx2)
    target_include_directories(quality_metrics_avx2 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/..")
    if(MSVC)
        target_compile_options(quality_metrics_avx2 PRIVATE /arch:AVX2)
    else()
        target_compile_options(quality_metrics_avx2 PRIVATE -mavx2)
    endif()
    target_sources(quality_metrics_lib PRIVATE
        $<TARGET_OBJECTS:quality_metrics_avx2>)
    target_compile_definitions(quality_metrics_lib PRIVATE
        VOID_QUALITY_HAS_AVX2_KERNEL=1)
endif()

add_library(analysis_lib STATIC
    ${VOID_ANALYSIS_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/quality/quality_video_analyzer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/quality/quality_wgpu_backend_stub.cpp"
)
void_apply_native_compile_options(analysis_lib)

target_sources(analysis_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../media/private_cdn_flv_demuxer.cpp
)

target_include_directories(analysis_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# AnalysisGenerator uses FFmpeg C API (avformat, avcodec)
if(FFMPEG_INCLUDE_DIR)
    target_include_directories(analysis_lib SYSTEM PUBLIC "${FFMPEG_INCLUDE_DIR}")
endif()

if(AVFORMAT_LIBRARY)
    target_link_libraries(analysis_lib PUBLIC
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
    )
endif()

target_link_libraries(analysis_lib PUBLIC
    void_video_decode_core
    quality_metrics_lib
    spdlog::spdlog_header_only
    Threads::Threads
)
void_link_zstd(analysis_lib)
