set(OUTPUT_BIN_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>")

# Copy FFmpeg DLLs early so test executables can run at build/test time.
if(EXISTS "${FFMPEG_BIN_DIR}")
    file(GLOB FFMPEG_DLL_FILES "${FFMPEG_BIN_DIR}/*.dll")
    set(FFMPEG_DLL_COPY_CMDS "")
    foreach(DLL ${FFMPEG_DLL_FILES})
        list(APPEND FFMPEG_DLL_COPY_CMDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "${OUTPUT_BIN_DIR}/")
    endforeach()
    add_custom_target(copy_ffmpeg_dlls ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_BIN_DIR}"
        ${FFMPEG_DLL_COPY_CMDS}
        COMMENT "Copying FFmpeg DLLs to output directory..."
    )
endif()
