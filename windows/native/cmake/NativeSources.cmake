include_guard(GLOBAL)

get_filename_component(VOID_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(VOID_RENDERER_SOURCES
    "${VOID_NATIVE_DIR}/video_renderer/renderer.cpp"
    "${VOID_NATIVE_DIR}/common/logging.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/clock.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/d3d11/device.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/d3d11/texture.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/d3d11/shader.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/demux_thread.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/decode_thread.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/frame_converter.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/hw/hw_decode_provider.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/hw/d3d11va_provider.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/buffer/packet_queue.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/buffer/bidi_ring_buffer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/buffer/track_buffer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/sync/render_sink.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/sync/seek_controller.cpp"
)

set(VOID_ANALYSIS_SOURCES
    "${VOID_NATIVE_DIR}/analysis/parsers/vbt_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/parsers/vbi_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/parsers/vbs2_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/analysis_manager.cpp"
    "${VOID_NATIVE_DIR}/analysis/generators/analysis_generator.cpp"
)

set(VOID_RENDERER_SHADER_SOURCE
    "${VOID_NATIVE_DIR}/video_renderer/shaders/multitrack.hlsl")
set(VOID_RENDERER_SHADER_TEMPLATE
    "${VOID_NATIVE_DIR}/video_renderer/shaders/embedded_shaders.h.in")

function(void_configure_renderer_shaders output_dir)
    file(READ "${VOID_RENDERER_SHADER_SOURCE}" MULTITRACK_HLSL)
    configure_file(
        "${VOID_RENDERER_SHADER_TEMPLATE}"
        "${output_dir}/embedded_shaders.h"
        @ONLY
    )
endfunction()
