include_guard(GLOBAL)

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
void_label_test(macos_media_smoke "macos;native;integration")

add_executable(software_bgra_converter_smoke
    "${VOID_NATIVE_DIR}/tools/software_bgra_converter_smoke.cpp"
)
void_apply_native_compile_options(software_bgra_converter_smoke)
target_link_libraries(software_bgra_converter_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME software_bgra_converter_smoke COMMAND software_bgra_converter_smoke)
void_label_test(software_bgra_converter_smoke "contract;portable")

add_executable(software_frame_packer_smoke
    "${VOID_NATIVE_DIR}/tools/software_frame_packer_smoke.cpp"
)
void_apply_native_compile_options(software_frame_packer_smoke)
target_link_libraries(software_frame_packer_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME software_frame_packer_smoke COMMAND software_frame_packer_smoke)
void_label_test(software_frame_packer_smoke "contract;portable")

add_executable(macos_presentation_adapter_smoke
    "${VOID_NATIVE_DIR}/tools/macos_presentation_adapter_smoke.cpp"
)
void_apply_native_compile_options(macos_presentation_adapter_smoke)
target_link_libraries(macos_presentation_adapter_smoke PRIVATE
    void_macos_native_player
)
add_test(NAME macos_presentation_adapter_smoke COMMAND macos_presentation_adapter_smoke)
void_label_test(macos_presentation_adapter_smoke "macos;backend;canary")

add_executable(macos_native_abi_smoke
    "${VOID_NATIVE_DIR}/tools/macos_native_abi_smoke.cpp"
)
void_apply_native_compile_options(macos_native_abi_smoke)
target_link_libraries(macos_native_abi_smoke PRIVATE
    void_macos_native_player
)
add_test(NAME macos_native_abi_smoke COMMAND macos_native_abi_smoke)
void_label_test(macos_native_abi_smoke "macos;abi;contract")

add_executable(macos_metal_contract_smoke
    "${VOID_NATIVE_DIR}/tools/macos_metal_contract_smoke.cpp"
)
void_apply_native_compile_options(macos_metal_contract_smoke)
target_link_libraries(macos_metal_contract_smoke PRIVATE
    void_macos_native_player
)
add_test(NAME macos_metal_contract_smoke COMMAND macos_metal_contract_smoke)
void_label_test(macos_metal_contract_smoke "macos;abi;backend;contract")

add_test(NAME macos_metal_shader_generated_check
    COMMAND ${CMAKE_COMMAND}
        -DVOID_NATIVE_DIR=${VOID_NATIVE_DIR}
        -P "${VOID_NATIVE_DIR}/cmake/CheckMacOSMetalShaderInc.cmake")
void_label_test(macos_metal_shader_generated_check "macos;shader;contract")

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
void_label_test(macos_metal_uploader_smoke "macos;backend;canary")

add_executable(macos_metal_presentation_backend_smoke
    "${VOID_NATIVE_DIR}/tools/macos_metal_presentation_backend_smoke.cpp"
)
void_apply_native_compile_options(macos_metal_presentation_backend_smoke)
target_link_libraries(macos_metal_presentation_backend_smoke PRIVATE
    void_macos_native_player
)
add_test(NAME macos_metal_presentation_backend_smoke COMMAND macos_metal_presentation_backend_smoke)
void_label_test(macos_metal_presentation_backend_smoke "macos;backend;canary")

add_executable(macos_metal_color_layout_parity_smoke
    "${VOID_NATIVE_DIR}/tools/macos_metal_color_layout_parity_smoke.cpp"
)
void_apply_native_compile_options(macos_metal_color_layout_parity_smoke)
target_link_libraries(macos_metal_color_layout_parity_smoke PRIVATE
    void_macos_native_player
)
add_test(NAME macos_metal_color_layout_parity_smoke
    COMMAND macos_metal_color_layout_parity_smoke)
void_label_test(macos_metal_color_layout_parity_smoke "macos;backend;contract")

add_executable(macos_hdr_sdr_compositor_demo
    "${VOID_NATIVE_DIR}/tools/macos_hdr_sdr_compositor_demo.mm"
)
void_apply_native_compile_options(macos_hdr_sdr_compositor_demo)
target_compile_options(macos_hdr_sdr_compositor_demo PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
target_link_libraries(macos_hdr_sdr_compositor_demo PRIVATE
    "-framework AppKit"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
)
add_test(NAME macos_hdr_sdr_compositor_headless
    COMMAND macos_hdr_sdr_compositor_demo --headless)
void_label_test(macos_hdr_sdr_compositor_headless "macos;hdr;backend;canary")

add_executable(macos_crash_handler_smoke
    "${VOID_NATIVE_DIR}/tools/macos_crash_handler_smoke.cpp"
)
void_apply_native_compile_options(macos_crash_handler_smoke)
target_link_libraries(macos_crash_handler_smoke PRIVATE
    void_macos_native_player
)
add_test(NAME macos_crash_handler_smoke COMMAND macos_crash_handler_smoke)
void_label_test(macos_crash_handler_smoke "macos;diagnostics;canary")

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
    if(FFMPEG_ANALYZER_PATH)
        set_tests_properties(macos_analysis_ffi_smoke PROPERTIES
            ENVIRONMENT "VOID_FFMPEG_ANALYZER=${FFMPEG_ANALYZER_PATH}")
    endif()
    void_label_test(macos_analysis_ffi_smoke "macos;analysis;ffi;canary")

    add_executable(analysis_generation_service_smoke
        "${VOID_NATIVE_DIR}/tools/analysis_generation_service_smoke.cpp"
    )
    void_apply_native_compile_options(analysis_generation_service_smoke)
    target_link_libraries(analysis_generation_service_smoke PRIVATE
        analysis_lib
    )
    add_test(NAME analysis_generation_service_smoke
        COMMAND analysis_generation_service_smoke)
    void_label_test(analysis_generation_service_smoke
        "analysis;contract;portable")

    if(FFMPEG_ANALYZER_PATH)
        add_executable(macos_analysis_toolchain_smoke
            "${VOID_NATIVE_DIR}/tools/macos_analysis_toolchain_smoke.cpp"
        )
        void_apply_native_compile_options(macos_analysis_toolchain_smoke)
        target_include_directories(macos_analysis_toolchain_smoke PRIVATE
            "${VOID_NATIVE_DIR}"
        )
        target_link_libraries(macos_analysis_toolchain_smoke PRIVATE
            analysis_lib
        )
        add_dependencies(macos_analysis_toolchain_smoke VoidPlayerCli)
        add_test(NAME macos_analysis_toolchain_smoke
            COMMAND macos_analysis_toolchain_smoke
                $<TARGET_FILE:VoidPlayerCli>
                "${FFMPEG_ANALYZER_PATH}"
                "${VIDEO_TEST_DIR}")
        void_label_test(macos_analysis_toolchain_smoke "macos;analysis;cli;integration")
    else()
        message(STATUS "macos_analysis_toolchain_smoke disabled: FFmpeg analyzer tool not found")
    endif()
endif()

add_executable(videotoolbox_provider_smoke
    "${VOID_NATIVE_DIR}/tools/videotoolbox_provider_smoke.cpp"
)
void_apply_native_compile_options(videotoolbox_provider_smoke)
target_link_libraries(videotoolbox_provider_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME videotoolbox_provider_smoke COMMAND videotoolbox_provider_smoke)
void_label_test(videotoolbox_provider_smoke "macos;videotoolbox;backend;canary")

add_executable(bgra_capture_metrics_smoke
    "${VOID_NATIVE_DIR}/tools/bgra_capture_metrics_smoke.cpp"
)
void_apply_native_compile_options(bgra_capture_metrics_smoke)
target_link_libraries(bgra_capture_metrics_smoke PRIVATE
    void_player_portable_core
)
add_test(NAME bgra_capture_metrics_smoke COMMAND bgra_capture_metrics_smoke)
void_label_test(bgra_capture_metrics_smoke "contract;portable")

add_executable(decoded_frame_sink_smoke
    "${VOID_NATIVE_DIR}/tools/decoded_frame_sink_smoke.cpp"
)
void_apply_native_compile_options(decoded_frame_sink_smoke)
target_link_libraries(decoded_frame_sink_smoke PRIVATE
    void_player_portable_core
)
add_test(NAME decoded_frame_sink_smoke COMMAND decoded_frame_sink_smoke)
void_label_test(decoded_frame_sink_smoke "contract;portable")

add_executable(layout_geometry_smoke
    "${VOID_NATIVE_DIR}/tools/layout_geometry_smoke.cpp"
)
void_apply_native_compile_options(layout_geometry_smoke)
target_link_libraries(layout_geometry_smoke PRIVATE
    void_player_portable_core
)
add_test(NAME layout_geometry_smoke COMMAND layout_geometry_smoke)
void_label_test(layout_geometry_smoke "contract;portable")

add_executable(renderer_config_validation_smoke
    "${VOID_NATIVE_DIR}/tools/renderer_config_validation_smoke.cpp"
)
void_apply_native_compile_options(renderer_config_validation_smoke)
target_link_libraries(renderer_config_validation_smoke PRIVATE
    void_player_portable_core
)
add_test(NAME renderer_config_validation_smoke COMMAND renderer_config_validation_smoke)
void_label_test(renderer_config_validation_smoke "contract;portable")

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
void_label_test(renderer_metal_headless_smoke "macos;backend;integration;hosted-flaky;nightly")

add_executable(presentation_snapshot_smoke
    "${VOID_NATIVE_DIR}/tools/presentation_snapshot_smoke.cpp"
)
void_apply_native_compile_options(presentation_snapshot_smoke)
target_link_libraries(presentation_snapshot_smoke PRIVATE
    void_player_portable_core
)
add_test(NAME presentation_snapshot_smoke COMMAND presentation_snapshot_smoke)
void_label_test(presentation_snapshot_smoke "contract;portable")

add_executable(presentation_carry_forward_smoke
    "${VOID_NATIVE_DIR}/tools/presentation_carry_forward_smoke.cpp"
)
void_apply_native_compile_options(presentation_carry_forward_smoke)
target_link_libraries(presentation_carry_forward_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME presentation_carry_forward_smoke COMMAND presentation_carry_forward_smoke)
void_label_test(presentation_carry_forward_smoke "contract;portable")

add_executable(av_frame_lifetime_smoke
    "${VOID_NATIVE_DIR}/tools/av_frame_lifetime_smoke.cpp"
)
void_apply_native_compile_options(av_frame_lifetime_smoke)
target_link_libraries(av_frame_lifetime_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME av_frame_lifetime_smoke COMMAND av_frame_lifetime_smoke)
void_label_test(av_frame_lifetime_smoke "contract;portable")

add_executable(software_frame_queue_smoke
    "${VOID_NATIVE_DIR}/tools/software_frame_queue_smoke.cpp"
)
void_apply_native_compile_options(software_frame_queue_smoke)
target_link_libraries(software_frame_queue_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME software_frame_queue_smoke COMMAND software_frame_queue_smoke)
void_label_test(software_frame_queue_smoke "contract;portable")

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
void_label_test(software_decode_frame_queue_smoke "macos;native;integration")

add_executable(decode_frame_drainer_smoke
    "${VOID_NATIVE_DIR}/tools/decode_frame_drainer_smoke.cpp"
)
void_apply_native_compile_options(decode_frame_drainer_smoke)
target_link_libraries(decode_frame_drainer_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME decode_frame_drainer_smoke COMMAND decode_frame_drainer_smoke)
void_label_test(decode_frame_drainer_smoke "contract;portable")

add_executable(decode_frame_receive_loop_smoke
    "${VOID_NATIVE_DIR}/tools/decode_frame_receive_loop_smoke.cpp"
)
void_apply_native_compile_options(decode_frame_receive_loop_smoke)
target_link_libraries(decode_frame_receive_loop_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME decode_frame_receive_loop_smoke COMMAND decode_frame_receive_loop_smoke)
void_label_test(decode_frame_receive_loop_smoke "contract;portable")

add_executable(decode_packet_sender_smoke
    "${VOID_NATIVE_DIR}/tools/decode_packet_sender_smoke.cpp"
)
void_apply_native_compile_options(decode_packet_sender_smoke)
target_link_libraries(decode_packet_sender_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME decode_packet_sender_smoke COMMAND decode_packet_sender_smoke)
void_label_test(decode_packet_sender_smoke "contract;portable")

add_executable(decode_exact_seek_reorder_smoke
    "${VOID_NATIVE_DIR}/tools/decode_exact_seek_reorder_smoke.cpp"
)
void_apply_native_compile_options(decode_exact_seek_reorder_smoke)
target_link_libraries(decode_exact_seek_reorder_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME decode_exact_seek_reorder_smoke COMMAND decode_exact_seek_reorder_smoke)
void_label_test(decode_exact_seek_reorder_smoke "contract;portable")

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
void_label_test(decode_thread_software_smoke "macos;native;integration")

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
void_label_test(macos_native_player_shared_renderer_smoke
    "macos;native;integration;videotoolbox;hosted-flaky;nightly")

add_executable(audio_mixer_smoke
    "${VOID_NATIVE_DIR}/tools/audio_mixer_smoke.cpp"
)
void_apply_native_compile_options(audio_mixer_smoke)
target_link_libraries(audio_mixer_smoke PRIVATE
    void_media_ffmpeg
)
add_test(NAME audio_mixer_smoke COMMAND audio_mixer_smoke)
void_label_test(audio_mixer_smoke "contract;portable;audio")
