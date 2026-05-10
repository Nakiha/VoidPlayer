include_guard(GLOBAL)

get_filename_component(VOID_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(VOID_RENDERER_CORE_SOURCES
    "${VOID_NATIVE_DIR}/common/logging.cpp"
    "${VOID_NATIVE_DIR}/common/windows_crash_handler.cpp"
    "${VOID_NATIVE_DIR}/playback/playback_controller.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/clock.cpp"
    "${VOID_NATIVE_DIR}/media/demux_thread.cpp"
    "${VOID_NATIVE_DIR}/media/private_cdn_flv_demuxer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/frame_converter.cpp"
    "${VOID_NATIVE_DIR}/media/packet_queue.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/buffer/bidi_ring_buffer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/buffer/track_buffer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/sync/render_sink.cpp"
    "${VOID_NATIVE_DIR}/media/seek_controller.cpp"
)

set(VOID_RENDERER_WINDOWS_SOURCES
    "${VOID_NATIVE_DIR}/audio/audio_engine.cpp"
    "${VOID_NATIVE_DIR}/audio/pcm_buffer.cpp"
    "${VOID_NATIVE_DIR}/audio/audio_output_factory.cpp"
    "${VOID_NATIVE_DIR}/player/native_player.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/audio_coordinator.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/seek_coordinator.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track_pipeline.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/renderer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/decode_thread.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/hw/hw_decode_provider.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/hw/d3d11va_provider.cpp"
)

set(VOID_D3D11_BACKEND_SOURCES
    "${VOID_NATIVE_DIR}/video_renderer/d3d11/device.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/d3d11/frame_presenter.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/d3d11/headless_output.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/d3d11/render_backend.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/d3d11/texture.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/d3d11/shader.cpp"
)

set(VOID_RENDERER_SOURCES
    ${VOID_RENDERER_CORE_SOURCES}
    ${VOID_RENDERER_WINDOWS_SOURCES}
    ${VOID_D3D11_BACKEND_SOURCES}
)

set(VOID_ANALYSIS_SOURCES
    "${VOID_NATIVE_DIR}/analysis/parsers/analysis_container.cpp"
    "${VOID_NATIVE_DIR}/analysis/parsers/vbt_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/parsers/vbi_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/parsers/vbs4_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/analysis_manager.cpp"
    "${VOID_NATIVE_DIR}/analysis/generators/bitstream_indexer.cpp"
    "${VOID_NATIVE_DIR}/analysis/generators/analysis_generator.cpp"
)

set(VOID_RENDERER_SHADER_SOURCES
    "${VOID_NATIVE_DIR}/video_renderer/shaders/common.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/color_pipeline.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/sampling.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/multitrack.hlsl")
set(VOID_RENDERER_SHADER_TEMPLATE
    "${VOID_NATIVE_DIR}/video_renderer/shaders/embedded_shaders.h.in")

function(void_apply_native_compile_options target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /utf-8 /W4 /WX /permissive- /EHsc)
    endif()
endfunction()

function(void_configure_renderer_shaders output_dir)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${VOID_RENDERER_SHADER_SOURCES})
    file(READ "${VOID_NATIVE_DIR}/video_renderer/shaders/common.hlsl" COMMON_HLSL)
    file(READ "${VOID_NATIVE_DIR}/video_renderer/shaders/color_pipeline.hlsl" COLOR_PIPELINE_HLSL)
    file(READ "${VOID_NATIVE_DIR}/video_renderer/shaders/sampling.hlsl" SAMPLING_HLSL)
    file(READ "${VOID_NATIVE_DIR}/video_renderer/shaders/multitrack.hlsl" MULTITRACK_HLSL)
    configure_file(
        "${VOID_RENDERER_SHADER_TEMPLATE}"
        "${output_dir}/embedded_shaders.h"
        @ONLY
    )
endfunction()
