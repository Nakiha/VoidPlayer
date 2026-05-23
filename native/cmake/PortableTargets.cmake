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
    )
endif()

if(APPLE)
    add_library(void_macos_native_player STATIC
        ${VOID_MACOS_NATIVE_PLAYER_SOURCES}
    )
    void_apply_native_compile_options(void_macos_native_player)
    target_include_directories(void_macos_native_player PUBLIC
        "${VOID_NATIVE_DIR}/macos"
    )
    target_link_libraries(void_macos_native_player PUBLIC
        void_media_ffmpeg
        "-framework Metal"
        "-framework CoreVideo"
    )
    target_compile_options(void_macos_native_player PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )

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

    add_executable(software_bgra_converter_smoke
        "${VOID_NATIVE_DIR}/tools/software_bgra_converter_smoke.cpp"
    )
    void_apply_native_compile_options(software_bgra_converter_smoke)
    target_link_libraries(software_bgra_converter_smoke PRIVATE
        void_media_ffmpeg
    )
    add_test(NAME software_bgra_converter_smoke COMMAND software_bgra_converter_smoke)

    add_executable(software_frame_packer_smoke
        "${VOID_NATIVE_DIR}/tools/software_frame_packer_smoke.cpp"
    )
    void_apply_native_compile_options(software_frame_packer_smoke)
    target_link_libraries(software_frame_packer_smoke PRIVATE
        void_media_ffmpeg
    )
    add_test(NAME software_frame_packer_smoke COMMAND software_frame_packer_smoke)

    add_executable(macos_presentation_adapter_smoke
        "${VOID_NATIVE_DIR}/tools/macos_presentation_adapter_smoke.cpp"
    )
    void_apply_native_compile_options(macos_presentation_adapter_smoke)
    target_link_libraries(macos_presentation_adapter_smoke PRIVATE
        void_macos_native_player
    )
    add_test(NAME macos_presentation_adapter_smoke COMMAND macos_presentation_adapter_smoke)

    add_executable(videotoolbox_provider_smoke
        "${VOID_NATIVE_DIR}/tools/videotoolbox_provider_smoke.cpp"
    )
    void_apply_native_compile_options(videotoolbox_provider_smoke)
    target_link_libraries(videotoolbox_provider_smoke PRIVATE
        void_media_ffmpeg
    )
    add_test(NAME videotoolbox_provider_smoke COMMAND videotoolbox_provider_smoke)

    add_executable(bgra_capture_metrics_smoke
        "${VOID_NATIVE_DIR}/tools/bgra_capture_metrics_smoke.cpp"
    )
    void_apply_native_compile_options(bgra_capture_metrics_smoke)
    target_link_libraries(bgra_capture_metrics_smoke PRIVATE
        void_player_portable_core
    )
    add_test(NAME bgra_capture_metrics_smoke COMMAND bgra_capture_metrics_smoke)

    add_executable(decoded_frame_sink_smoke
        "${VOID_NATIVE_DIR}/tools/decoded_frame_sink_smoke.cpp"
    )
    void_apply_native_compile_options(decoded_frame_sink_smoke)
    target_link_libraries(decoded_frame_sink_smoke PRIVATE
        void_player_portable_core
    )
    add_test(NAME decoded_frame_sink_smoke COMMAND decoded_frame_sink_smoke)

    add_executable(software_frame_queue_smoke
        "${VOID_NATIVE_DIR}/tools/software_frame_queue_smoke.cpp"
    )
    void_apply_native_compile_options(software_frame_queue_smoke)
    target_link_libraries(software_frame_queue_smoke PRIVATE
        void_media_ffmpeg
    )
    add_test(NAME software_frame_queue_smoke COMMAND software_frame_queue_smoke)

    add_executable(software_decode_frame_queue_smoke
        "${VOID_NATIVE_DIR}/tools/software_decode_frame_queue_smoke.cpp"
    )
    void_apply_native_compile_options(software_decode_frame_queue_smoke)
    target_link_libraries(software_decode_frame_queue_smoke PRIVATE
        void_media_ffmpeg
    )
    target_compile_definitions(software_decode_frame_queue_smoke PRIVATE
        VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
    )
    add_test(NAME software_decode_frame_queue_smoke COMMAND software_decode_frame_queue_smoke)

    add_executable(decode_thread_software_smoke
        "${VOID_NATIVE_DIR}/tools/decode_thread_software_smoke.cpp"
    )
    void_apply_native_compile_options(decode_thread_software_smoke)
    target_link_libraries(decode_thread_software_smoke PRIVATE
        void_media_ffmpeg
    )
    target_compile_definitions(decode_thread_software_smoke PRIVATE
        VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
    )
    add_test(NAME decode_thread_software_smoke COMMAND decode_thread_software_smoke)

    add_executable(macos_native_player_smoke
        "${VOID_NATIVE_DIR}/tools/macos_native_player_smoke.cpp"
    )
    void_apply_native_compile_options(macos_native_player_smoke)
    target_link_libraries(macos_native_player_smoke PRIVATE
        void_macos_native_player
    )
    target_compile_definitions(macos_native_player_smoke PRIVATE
        VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
    )
    add_test(NAME macos_native_player_smoke COMMAND macos_native_player_smoke)

    add_executable(audio_mixer_smoke
        "${VOID_NATIVE_DIR}/tools/audio_mixer_smoke.cpp"
    )
    void_apply_native_compile_options(audio_mixer_smoke)
    target_link_libraries(audio_mixer_smoke PRIVATE
        void_media_ffmpeg
    )
    add_test(NAME audio_mixer_smoke COMMAND audio_mixer_smoke)

endif()
