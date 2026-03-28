set(VR_SOURCES
    renderer.cpp
    logging.cpp
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

# Copy shaders to build output and use build-relative path for deployment
set(VR_SHADER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${VR_SHADER_BUILD_DIR}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/shaders/" DESTINATION "${VR_SHADER_BUILD_DIR}")
target_compile_definitions(video_renderer_lib PRIVATE
    VR_SHADER_DIR="${VR_SHADER_BUILD_DIR}"
)
