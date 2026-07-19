include_guard(GLOBAL)

set(_VOID_VIDEO_DECODE_CORE_SOURCES
    "${VOID_NATIVE_DIR}/media/ffmpeg_lifetime.cpp"
    "${VOID_NATIVE_DIR}/media/video_decode_session.cpp"
    "${VOID_NATIVE_DIR}/renderer/decode/codec_loop.cpp"
    "${VOID_NATIVE_DIR}/renderer/decode/hw/hw_decode_provider.cpp"
)

if(WIN32)
    list(APPEND _VOID_VIDEO_DECODE_CORE_SOURCES
        "${VOID_NATIVE_DIR}/windows/decode/d3d11va_provider.cpp")
elseif(APPLE)
    list(APPEND _VOID_VIDEO_DECODE_CORE_SOURCES
        "${VOID_NATIVE_DIR}/macos/decode/videotoolbox_provider.cpp")
endif()

add_library(void_video_decode_core STATIC
    ${_VOID_VIDEO_DECODE_CORE_SOURCES}
)
void_apply_native_compile_options(void_video_decode_core)

target_include_directories(void_video_decode_core PUBLIC
    "${VOID_NATIVE_DIR}"
)
target_include_directories(void_video_decode_core SYSTEM PUBLIC
    "${FFMPEG_INCLUDE_DIR}"
)
target_link_libraries(void_video_decode_core PUBLIC
    spdlog::spdlog_header_only
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
)

if(WIN32)
    target_link_libraries(void_video_decode_core PUBLIC d3d11 dxgi)
elseif(APPLE)
    target_link_libraries(void_video_decode_core PUBLIC
        "-framework CoreVideo"
    )
endif()
