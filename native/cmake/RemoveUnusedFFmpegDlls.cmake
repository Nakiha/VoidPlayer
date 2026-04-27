if(NOT DEFINED VOID_FFMPEG_RUNTIME_DIR)
    message(FATAL_ERROR "VOID_FFMPEG_RUNTIME_DIR is required")
endif()

file(GLOB _void_unused_ffmpeg_dlls
    "${VOID_FFMPEG_RUNTIME_DIR}/avfilter-*.dll"
    "${VOID_FFMPEG_RUNTIME_DIR}/avdevice-*.dll")

if(_void_unused_ffmpeg_dlls)
    file(REMOVE ${_void_unused_ffmpeg_dlls})
endif()
