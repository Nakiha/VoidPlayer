include_guard(GLOBAL)

add_library(void_player_portable_core STATIC
    ${VOID_PLAYER_PORTABLE_CORE_SOURCES}
)
void_apply_native_compile_options(void_player_portable_core)

target_include_directories(void_player_portable_core PUBLIC
    "${VOID_NATIVE_DIR}"
)
target_include_directories(void_player_portable_core SYSTEM PUBLIC
    "${FFMPEG_INCLUDE_DIR}"
)

target_link_libraries(void_player_portable_core PUBLIC
    spdlog::spdlog_header_only
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
)

add_library(void_media_ffmpeg STATIC
    ${VOID_MEDIA_FFMPEG_SOURCES}
)
void_apply_native_compile_options(void_media_ffmpeg)

target_include_directories(void_media_ffmpeg PUBLIC
    "${VOID_NATIVE_DIR}"
)
target_include_directories(void_media_ffmpeg SYSTEM PUBLIC
    "${FFMPEG_INCLUDE_DIR}"
    "${VOID_MINIAUDIO_INCLUDE_DIR}"
)

target_link_libraries(void_media_ffmpeg PUBLIC
    void_player_portable_core
    spdlog::spdlog_header_only
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
)

if(APPLE)
    target_link_libraries(void_media_ffmpeg PUBLIC
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreFoundation"
        "-framework CoreVideo"
    )
endif()

function(void_label_test test_name labels)
    set_tests_properties("${test_name}" PROPERTIES LABELS "${labels}")
endfunction()

if(APPLE)
    include(cmake/MacOSTargets.cmake)
    if(BUILD_TESTS)
        include(cmake/MacOSTests.cmake)
    endif()
endif()
