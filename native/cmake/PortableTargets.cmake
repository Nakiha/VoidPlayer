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
)

target_link_libraries(void_media_ffmpeg PUBLIC
    void_player_portable_core
    spdlog::spdlog_header_only
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
)

if(APPLE)
    add_executable(macos_media_smoke
        "${VOID_NATIVE_DIR}/tools/macos_media_smoke.cpp"
    )
    void_apply_native_compile_options(macos_media_smoke)
    target_link_libraries(macos_media_smoke PRIVATE
        void_media_ffmpeg
        spdlog::spdlog_header_only
    )
    target_compile_definitions(macos_media_smoke PRIVATE
        VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
    )
    add_test(NAME macos_media_smoke COMMAND macos_media_smoke)
endif()
