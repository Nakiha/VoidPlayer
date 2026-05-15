set(DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Native staging directory")

if(TARGET video_renderer_ffi)
    file(MAKE_DIRECTORY "${DIST_DIR}/ffi")

    add_custom_command(TARGET video_renderer_ffi POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/ffi"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:video_renderer_ffi>"
            "${DIST_DIR}/ffi/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:video_renderer_ffi>/$<TARGET_FILE_PREFIX:video_renderer_ffi>$<TARGET_FILE_BASE_NAME:video_renderer_ffi>.lib"
            "${DIST_DIR}/ffi/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/video_renderer/exports/ffi_exports.h"
            "${DIST_DIR}/ffi/"
        COMMENT "Installing FFI artifacts to ${DIST_DIR}/ffi/..."
    )

    if(EXISTS "${FFMPEG_BIN_DIR}")
        void_collect_ffmpeg_runtime_dlls(FFMPEG_FFI_DLL_FILES)
        void_collect_ffmpeg_notice_files(FFMPEG_FFI_NOTICE_FILES)
        set(FFI_DLL_COPY_CMDS "")
        foreach(DLL ${FFMPEG_FFI_DLL_FILES})
            list(APPEND FFI_DLL_COPY_CMDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "${DIST_DIR}/ffi/")
        endforeach()
        foreach(NOTICE ${FFMPEG_FFI_NOTICE_FILES})
            list(APPEND FFI_DLL_COPY_CMDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NOTICE}" "${DIST_DIR}/ffi/")
        endforeach()
        add_custom_target(copy_ffmpeg_to_ffi_dist ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/ffi"
            COMMAND ${CMAKE_COMMAND}
                -DVOID_FFMPEG_RUNTIME_DIR="${DIST_DIR}/ffi"
                -P "${VOID_NATIVE_DIR}/cmake/RemoveUnusedFFmpegDlls.cmake"
            ${FFI_DLL_COPY_CMDS}
            COMMENT "Copying FFmpeg DLLs to ${DIST_DIR}/ffi/..."
        )
        add_dependencies(copy_ffmpeg_to_ffi_dist video_renderer_ffi)
        if(TARGET copy_ffmpeg_dlls)
            add_dependencies(copy_ffmpeg_to_ffi_dist copy_ffmpeg_dlls)
        endif()
    endif()
endif()

if(BUILD_PYTHON AND TARGET video_renderer_native)
    file(MAKE_DIRECTORY "${DIST_DIR}/python")
    file(MAKE_DIRECTORY "${DIST_DIR}/python/video_renderer")

    add_custom_command(TARGET video_renderer_native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/python/video_renderer"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:video_renderer_native>"
            "${DIST_DIR}/python/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/video_renderer/exports/__init__.py"
            "${DIST_DIR}/python/video_renderer/__init__.py"
        COMMENT "Installing Python artifacts to ${DIST_DIR}/python/..."
    )

    if(EXISTS "${FFMPEG_BIN_DIR}")
        void_collect_ffmpeg_runtime_dlls(FFMPEG_DLL_FILES)
        void_collect_ffmpeg_notice_files(FFMPEG_NOTICE_FILES)
        set(DIST_DLL_COPY_CMDS "")
        foreach(DLL ${FFMPEG_DLL_FILES})
            list(APPEND DIST_DLL_COPY_CMDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "${DIST_DIR}/python/")
        endforeach()
        foreach(NOTICE ${FFMPEG_NOTICE_FILES})
            list(APPEND DIST_DLL_COPY_CMDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NOTICE}" "${DIST_DIR}/python/")
        endforeach()
        add_custom_target(copy_ffmpeg_to_dist ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/python"
            COMMAND ${CMAKE_COMMAND}
                -DVOID_FFMPEG_RUNTIME_DIR="${DIST_DIR}/python"
                -P "${VOID_NATIVE_DIR}/cmake/RemoveUnusedFFmpegDlls.cmake"
            ${DIST_DLL_COPY_CMDS}
            COMMENT "Copying FFmpeg DLLs to ${DIST_DIR}/python/..."
        )
        if(TARGET copy_ffmpeg_dlls)
            add_dependencies(copy_ffmpeg_to_dist copy_ffmpeg_dlls)
        endif()
    endif()
endif()
