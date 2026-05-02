add_library(analysis_lib STATIC ${VOID_ANALYSIS_SOURCES})
void_apply_native_compile_options(analysis_lib)

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

add_executable(analysis_generate
    tools/analysis_generate.cpp
)
void_apply_native_compile_options(analysis_generate)

target_link_libraries(analysis_generate PRIVATE
    analysis_lib
)

set_target_properties(analysis_generate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

if(TARGET copy_ffmpeg_dlls)
    add_dependencies(analysis_generate copy_ffmpeg_dlls)
endif()
