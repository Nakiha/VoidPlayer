include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/NativeSources.cmake")

set(VOID_FLUTTER_ZSTD_DIR "${VOID_NATIVE_DIR}/analysis/vendor/zstd")
if(BUILD_ANALYSIS AND EXISTS "${VOID_FLUTTER_ZSTD_DIR}/build/cmake/CMakeLists.txt" AND NOT TARGET libzstd_static)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_USE_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
    add_subdirectory("${VOID_FLUTTER_ZSTD_DIR}/build/cmake"
                     "${CMAKE_BINARY_DIR}/_deps/zstd-build"
                     EXCLUDE_FROM_ALL)
endif()

function(void_configure_flutter_native_target target_name generated_include_dir)
    void_apply_native_compile_options(${target_name})

    target_sources(${target_name} PRIVATE
        ${VOID_RENDERER_CORE_SOURCES}
        ${VOID_RENDERER_WINDOWS_SOURCES}
        ${VOID_D3D11_BACKEND_SOURCES}
    )
    if(BUILD_ANALYSIS)
        target_sources(${target_name} PRIVATE ${VOID_ANALYSIS_SOURCES})
    endif()

    target_include_directories(${target_name} PRIVATE
        "${VOID_NATIVE_DIR}"
        "${generated_include_dir}"
    )
    target_include_directories(${target_name} SYSTEM PRIVATE
        "${FFMPEG_INCLUDE_DIR}"
    )

    target_link_libraries(${target_name} PRIVATE
        spdlog::spdlog_header_only
        ${AVCODEC_LIBRARY}
        ${AVFORMAT_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        dxgi
        d3d11
        d3dcompiler
        winmm
    )
    if(BUILD_ANALYSIS)
        target_link_libraries(${target_name} PRIVATE libzstd_static)
    endif()

    target_compile_definitions(${target_name} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        VOID_BUILD_ANALYSIS=$<BOOL:${BUILD_ANALYSIS}>
    )

    void_configure_renderer_shaders("${generated_include_dir}")

    set(FFMPEG_DLL_DIR "${FFMPEG_ROOT}/bin")
    if(EXISTS "${FFMPEG_DLL_DIR}")
        void_collect_ffmpeg_runtime_dlls(FFMPEG_DLL_FILES)
        void_collect_ffmpeg_notice_files(FFMPEG_NOTICE_FILES)
        foreach(DLL ${FFMPEG_DLL_FILES})
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL}" "$<TARGET_FILE_DIR:${target_name}>"
            )
        endforeach()
        foreach(NOTICE ${FFMPEG_NOTICE_FILES})
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${NOTICE}" "$<TARGET_FILE_DIR:${target_name}>"
            )
        endforeach()
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DVOID_FFMPEG_RUNTIME_DIR="$<TARGET_FILE_DIR:${target_name}>"
                -P "${VOID_NATIVE_DIR}/cmake/RemoveUnusedFFmpegDlls.cmake"
        )
    endif()
endfunction()
