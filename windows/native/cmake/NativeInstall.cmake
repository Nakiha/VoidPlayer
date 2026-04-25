set(DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist")

file(MAKE_DIRECTORY "${DIST_DIR}/python")
file(MAKE_DIRECTORY "${DIST_DIR}/python/video_renderer")
file(MAKE_DIRECTORY "${DIST_DIR}/ffi")

if(TARGET video_renderer_ffi)
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
        COMMENT "Installing FFI artifacts to dist/ffi/..."
    )
endif()

if(BUILD_PYTHON AND TARGET video_renderer_native)
    add_custom_command(TARGET video_renderer_native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/python/video_renderer"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:video_renderer_native>"
            "${DIST_DIR}/python/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/video_renderer/exports/__init__.py"
            "${DIST_DIR}/python/video_renderer/__init__.py"
        COMMENT "Installing Python artifacts to dist/python/..."
    )

    if(EXISTS "${FFMPEG_BIN_DIR}")
        file(GLOB FFMPEG_DLL_FILES "${FFMPEG_BIN_DIR}/*.dll")
        set(DIST_DLL_COPY_CMDS "")
        foreach(DLL ${FFMPEG_DLL_FILES})
            list(APPEND DIST_DLL_COPY_CMDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "${DIST_DIR}/python/")
        endforeach()
        add_custom_target(copy_ffmpeg_to_dist ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/python"
            ${DIST_DLL_COPY_CMDS}
            COMMENT "Copying FFmpeg DLLs to dist/python/..."
        )
        if(TARGET copy_ffmpeg_dlls)
            add_dependencies(copy_ffmpeg_to_dist copy_ffmpeg_dlls)
        endif()
    endif()
endif()
