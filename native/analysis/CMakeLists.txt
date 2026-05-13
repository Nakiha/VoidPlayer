add_library(analysis_lib STATIC ${VOID_ANALYSIS_SOURCES})
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
    spdlog::spdlog_header_only
)
void_link_zstd(analysis_lib)
