set(OUTPUT_BIN_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>")

# Copy FFmpeg DLLs early so test executables can run at build/test time.
if(EXISTS "${FFMPEG_BIN_DIR}")
    void_collect_ffmpeg_runtime_dlls(FFMPEG_DLL_FILES)
    set(FFMPEG_DLL_COPY_CMDS "")
    foreach(DLL ${FFMPEG_DLL_FILES})
        list(APPEND FFMPEG_DLL_COPY_CMDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "${OUTPUT_BIN_DIR}/")
    endforeach()
    add_custom_target(copy_ffmpeg_dlls ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_BIN_DIR}"
        COMMAND ${CMAKE_COMMAND}
            -DVOID_FFMPEG_RUNTIME_DIR="${OUTPUT_BIN_DIR}"
            -P "${VOID_NATIVE_DIR}/cmake/RemoveUnusedFFmpegDlls.cmake"
        ${FFMPEG_DLL_COPY_CMDS}
        COMMENT "Copying FFmpeg DLLs to output directory..."
    )
endif()
