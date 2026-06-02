include_guard(GLOBAL)

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
