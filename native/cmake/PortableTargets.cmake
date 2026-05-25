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

if(APPLE)
    add_library(void_renderer_portable_driver OBJECT
        "${VOID_NATIVE_DIR}/video_renderer/renderer.cpp"
        ${VOID_RENDERER_PORTABLE_DRIVER_SOURCES}
        "${VOID_NATIVE_DIR}/video_renderer/overlay/analysis_overlay_renderer_portable_stub.cpp"
    )
    void_apply_native_compile_options(void_renderer_portable_driver)
    target_link_libraries(void_renderer_portable_driver PRIVATE
        void_media_ffmpeg
    )

    add_library(void_macos_native_player STATIC
        ${VOID_MACOS_NATIVE_PLAYER_SOURCES}
        $<TARGET_OBJECTS:void_renderer_portable_driver>
    )
    void_apply_native_compile_options(void_macos_native_player)
    target_include_directories(void_macos_native_player PUBLIC
        "${VOID_NATIVE_DIR}/macos"
    )
    target_link_libraries(void_macos_native_player PUBLIC
        void_media_ffmpeg
        "-framework Foundation"
        "-framework Metal"
        "-framework CoreVideo"
    )
    if(BUILD_ANALYSIS)
        target_link_libraries(void_macos_native_player PUBLIC analysis_lib)
    endif()
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

    add_executable(macos_metal_uploader_smoke
        "${VOID_NATIVE_DIR}/tools/macos_metal_uploader_smoke.mm"
    )
    void_apply_native_compile_options(macos_metal_uploader_smoke)
    target_compile_options(macos_metal_uploader_smoke PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_link_libraries(macos_metal_uploader_smoke PRIVATE
        void_macos_native_player
    )
    target_compile_definitions(macos_metal_uploader_smoke PRIVATE
        VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
    )
    add_test(NAME macos_metal_uploader_smoke COMMAND macos_metal_uploader_smoke)

    add_executable(macos_metal_presentation_backend_smoke
        "${VOID_NATIVE_DIR}/tools/macos_metal_presentation_backend_smoke.cpp"
    )
    void_apply_native_compile_options(macos_metal_presentation_backend_smoke)
    target_link_libraries(macos_metal_presentation_backend_smoke PRIVATE
        void_macos_native_player
    )
    add_test(NAME macos_metal_presentation_backend_smoke COMMAND macos_metal_presentation_backend_smoke)

    add_executable(macos_crash_handler_smoke
        "${VOID_NATIVE_DIR}/tools/macos_crash_handler_smoke.cpp"
    )
    void_apply_native_compile_options(macos_crash_handler_smoke)
    target_link_libraries(macos_crash_handler_smoke PRIVATE
        void_macos_native_player
    )
    add_test(NAME macos_crash_handler_smoke COMMAND macos_crash_handler_smoke)

    if(BUILD_ANALYSIS)
        add_executable(macos_analysis_ffi_smoke
            "${VOID_NATIVE_DIR}/tools/macos_analysis_ffi_smoke.cpp"
        )
        void_apply_native_compile_options(macos_analysis_ffi_smoke)
        target_link_libraries(macos_analysis_ffi_smoke PRIVATE
            void_macos_native_player
        )
        target_compile_definitions(macos_analysis_ffi_smoke PRIVATE
            VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
        )
        add_test(NAME macos_analysis_ffi_smoke COMMAND macos_analysis_ffi_smoke)
    endif()

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

    add_executable(layout_geometry_smoke
        "${VOID_NATIVE_DIR}/tools/layout_geometry_smoke.cpp"
    )
    void_apply_native_compile_options(layout_geometry_smoke)
    target_link_libraries(layout_geometry_smoke PRIVATE
        void_player_portable_core
    )
    add_test(NAME layout_geometry_smoke COMMAND layout_geometry_smoke)

    add_executable(renderer_config_validation_smoke
        "${VOID_NATIVE_DIR}/tools/renderer_config_validation_smoke.cpp"
    )
    void_apply_native_compile_options(renderer_config_validation_smoke)
    target_link_libraries(renderer_config_validation_smoke PRIVATE
        void_player_portable_core
    )
    add_test(NAME renderer_config_validation_smoke COMMAND renderer_config_validation_smoke)

    add_custom_target(renderer_portable_compile_smoke
        DEPENDS void_renderer_portable_driver
    )

    add_executable(renderer_metal_headless_smoke
        "${VOID_NATIVE_DIR}/tools/renderer_metal_headless_smoke.cpp"
    )
    void_apply_native_compile_options(renderer_metal_headless_smoke)
    target_link_libraries(renderer_metal_headless_smoke PRIVATE
        void_macos_native_player
    )
    target_compile_definitions(renderer_metal_headless_smoke PRIVATE
        VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
    )
    add_test(NAME renderer_metal_headless_smoke COMMAND renderer_metal_headless_smoke)

    add_executable(presentation_snapshot_smoke
        "${VOID_NATIVE_DIR}/tools/presentation_snapshot_smoke.cpp"
    )
    void_apply_native_compile_options(presentation_snapshot_smoke)
    target_link_libraries(presentation_snapshot_smoke PRIVATE
        void_player_portable_core
    )
    add_test(NAME presentation_snapshot_smoke COMMAND presentation_snapshot_smoke)

    add_executable(presentation_loop_driver_smoke
        "${VOID_NATIVE_DIR}/tools/presentation_loop_driver_smoke.cpp"
    )
    void_apply_native_compile_options(presentation_loop_driver_smoke)
    target_link_libraries(presentation_loop_driver_smoke PRIVATE
        void_player_portable_core
    )
    add_test(NAME presentation_loop_driver_smoke COMMAND presentation_loop_driver_smoke)

    add_executable(presentation_carry_forward_smoke
        "${VOID_NATIVE_DIR}/tools/presentation_carry_forward_smoke.cpp"
    )
    void_apply_native_compile_options(presentation_carry_forward_smoke)
    target_link_libraries(presentation_carry_forward_smoke PRIVATE
        void_media_ffmpeg
    )
    add_test(NAME presentation_carry_forward_smoke COMMAND presentation_carry_forward_smoke)

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

    add_executable(macos_native_player_shared_renderer_smoke
        "${VOID_NATIVE_DIR}/tools/macos_native_player_shared_renderer_smoke.cpp"
    )
    void_apply_native_compile_options(macos_native_player_shared_renderer_smoke)
    target_link_libraries(macos_native_player_shared_renderer_smoke PRIVATE
        void_macos_native_player
    )
    target_compile_definitions(macos_native_player_shared_renderer_smoke PRIVATE
        VIDEO_TEST_DIR="${VIDEO_TEST_DIR}"
    )
    add_test(NAME macos_native_player_shared_renderer_smoke
        COMMAND macos_native_player_shared_renderer_smoke)

    add_executable(audio_mixer_smoke
        "${VOID_NATIVE_DIR}/tools/audio_mixer_smoke.cpp"
    )
    void_apply_native_compile_options(audio_mixer_smoke)
    target_link_libraries(audio_mixer_smoke PRIVATE
        void_media_ffmpeg
    )
    add_test(NAME audio_mixer_smoke COMMAND audio_mixer_smoke)

endif()
