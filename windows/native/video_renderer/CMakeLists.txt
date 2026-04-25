set(VR_SOURCES
    renderer.cpp
    ../common/logging.cpp
    clock.cpp
    d3d11/device.cpp
    d3d11/texture.cpp
    d3d11/shader.cpp
    decode/demux_thread.cpp
    decode/decode_thread.cpp
    decode/frame_converter.cpp
    decode/hw/hw_decode_provider.cpp
    decode/hw/d3d11va_provider.cpp
    buffer/packet_queue.cpp
    buffer/bidi_ring_buffer.cpp
    buffer/track_buffer.cpp
    sync/render_sink.cpp
    sync/seek_controller.cpp
)

add_library(video_renderer_lib STATIC ${VR_SOURCES})

target_include_directories(video_renderer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(video_renderer_lib PUBLIC
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

# Embed shader source into binary (no runtime file dependency)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/multitrack.hlsl" MULTITRACK_HLSL)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/embedded_shaders.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders.h"
    @ONLY
)
