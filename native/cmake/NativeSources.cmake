include_guard(GLOBAL)

get_filename_component(VOID_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VOID_MINIAUDIO_INCLUDE_DIR "${VOID_NATIVE_DIR}/../third_party/miniaudio/include")

option(BUILD_ANALYSIS "Build native analysis cache, CLI, and overlay feature" ON)

set(VOID_RENDERER_CORE_SOURCES
    "${VOID_NATIVE_DIR}/common/logging.cpp"
    "${VOID_NATIVE_DIR}/common/windows_crash_handler.cpp"
    "${VOID_NATIVE_DIR}/playback/playback_controller.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/clock.cpp"
    "${VOID_NATIVE_DIR}/media/demux_thread.cpp"
    "${VOID_NATIVE_DIR}/media/private_cdn_flv_demuxer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/frame_converter.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/decode_loop_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/exact_seek_window.cpp"
    "${VOID_NATIVE_DIR}/media/packet_queue.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/buffer/bidi_ring_buffer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/buffer/track_buffer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/sync/render_sink.cpp"
    "${VOID_NATIVE_DIR}/media/seek_controller.cpp"
)

set(VOID_RENDERER_WINDOWS_SOURCES
    "${VOID_NATIVE_DIR}/audio/audio_decode_thread.cpp"
    "${VOID_NATIVE_DIR}/audio/audio_engine.cpp"
    "${VOID_NATIVE_DIR}/audio/audio_mixer.cpp"
    "${VOID_NATIVE_DIR}/audio/audio_track_registry.cpp"
    "${VOID_NATIVE_DIR}/audio/pcm_buffer.cpp"
    "${VOID_NATIVE_DIR}/audio/miniaudio_output.cpp"
    "${VOID_NATIVE_DIR}/audio/audio_output_factory.cpp"
    "${VOID_NATIVE_DIR}/player/native_player.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/audio_coordinator.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/capture/frame_capture_service.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/layout/layout_controller.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/layout/layout_geometry.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/render/render_loop_controller.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/renderer_config_validation.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/playback/renderer_playback_command_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/seek/renderer_seek_log_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/seek/seek_coordinator.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_buffer_budget.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_lifecycle.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_pipeline.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_pipeline_factory.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_perf_baseline.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_preroll_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_present_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_preview_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_snapshot.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/track/track_step_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/renderer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/av_frame_lifetime.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/codec_loop.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/decode_drain_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/decode_preroll_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/decode_seek_epoch.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/decoded_frame_publisher.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/decode_thread.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/exact_seek_candidate_store.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/exact_seek_frame_publisher.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/exact_seek_publish_policy.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/frame_timestamp_rescaler.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/hw/hw_decode_provider.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/hw/d3d11va_provider.cpp"
)

if(BUILD_ANALYSIS)
    list(APPEND VOID_RENDERER_WINDOWS_SOURCES
        "${VOID_NATIVE_DIR}/video_renderer/overlay/analysis_overlay_renderer.cpp")
else()
    list(APPEND VOID_RENDERER_WINDOWS_SOURCES
        "${VOID_NATIVE_DIR}/video_renderer/overlay/analysis_overlay_renderer_stub.cpp")
endif()

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
    "${VOID_NATIVE_DIR}/analysis/cache/overlay_chunk.cpp"
    "${VOID_NATIVE_DIR}/analysis/cache/overlay_raster.cpp"
    "${VOID_NATIVE_DIR}/analysis/cache/vacache_store.cpp"
    "${VOID_NATIVE_DIR}/analysis/parsers/vac2_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/parsers/vachunk_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/analysis_manager.cpp"
    "${VOID_NATIVE_DIR}/analysis/analysis_overlay_track_registry.cpp"
    "${VOID_NATIVE_DIR}/analysis/analysis_session.cpp"
    "${VOID_NATIVE_DIR}/analysis/generators/bitstream_indexer.cpp"
    "${VOID_NATIVE_DIR}/analysis/generators/analysis_generator.cpp"
)

set(VOID_RENDERER_SHADER_SOURCES
    "${VOID_NATIVE_DIR}/video_renderer/shaders/common.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/color_pipeline.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/sampling.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/multitrack.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay_invert.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay_contrast.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay_rect.hlsl"
    "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay_mask_rect.hlsl")
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
    file(READ "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay.hlsl" ANALYSIS_OVERLAY_HLSL)
    file(READ "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay_invert.hlsl" ANALYSIS_OVERLAY_INVERT_HLSL)
    file(READ "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay_contrast.hlsl" ANALYSIS_OVERLAY_CONTRAST_HLSL)
    file(READ "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay_rect.hlsl" ANALYSIS_OVERLAY_RECT_HLSL)
    file(READ "${VOID_NATIVE_DIR}/video_renderer/shaders/analysis_overlay_mask_rect.hlsl" ANALYSIS_OVERLAY_MASK_RECT_HLSL)
    configure_file(
        "${VOID_RENDERER_SHADER_TEMPLATE}"
        "${output_dir}/embedded_shaders.h"
        @ONLY
    )
endfunction()
