include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/NativeSources.cmake")

function(void_configure_flutter_native_target target_name generated_include_dir)
    target_sources(${target_name} PRIVATE
        ${VOID_RENDERER_SOURCES}
        ${VOID_ANALYSIS_SOURCES}
    )

    target_include_directories(${target_name} PRIVATE
        "${VOID_NATIVE_DIR}"
        "${generated_include_dir}"
        "${FFMPEG_INCLUDE_DIR}"
    )

    target_link_libraries(${target_name} PRIVATE
        spdlog::spdlog_header_only
        ${AVCODEC_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${SWSCALE_LIBRARY}
        dxgi
        d3d11
        d3dcompiler
    )

    target_compile_definitions(${target_name} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )

    void_configure_renderer_shaders("${generated_include_dir}")

    set(FFMPEG_DLL_DIR "${FFMPEG_ROOT}/bin")
    if(EXISTS "${FFMPEG_DLL_DIR}")
        void_collect_ffmpeg_runtime_dlls(FFMPEG_DLL_FILES)
        foreach(DLL ${FFMPEG_DLL_FILES})
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL}" "$<TARGET_FILE_DIR:${target_name}>"
            )
        endforeach()
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DVOID_FFMPEG_RUNTIME_DIR="$<TARGET_FILE_DIR:${target_name}>"
                -P "${VOID_NATIVE_DIR}/cmake/RemoveUnusedFFmpegDlls.cmake"
        )
    endif()
endfunction()
