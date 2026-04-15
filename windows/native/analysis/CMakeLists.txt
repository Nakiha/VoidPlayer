set(ANALYSIS_SOURCES
    parsers/vbt_parser.cpp
    parsers/vbi_parser.cpp
    parsers/vbs2_parser.cpp
    analysis_manager.cpp
    generators/analysis_generator.cpp
)

add_library(analysis_lib STATIC ${ANALYSIS_SOURCES})

target_include_directories(analysis_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# AnalysisGenerator uses FFmpeg C API (avformat, avcodec)
if(FFMPEG_INCLUDE_DIR)
    target_include_directories(analysis_lib PUBLIC "${FFMPEG_INCLUDE_DIR}")
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
