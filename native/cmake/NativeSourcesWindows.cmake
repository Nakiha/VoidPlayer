include_guard(GLOBAL)

set(VOID_RENDERER_WINDOWS_SOURCES
    "${VOID_NATIVE_DIR}/windows/common/windows_crash_handler.cpp"
    "${VOID_NATIVE_DIR}/windows/player/native_player.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_dcomp_composite.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_device_recovery.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_display_resolver.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_d3d12_present_target.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_high_refresh_metrics.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_overlay_layer_state.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_presentation_policy.cpp"
    "${VOID_NATIVE_DIR}/windows/shared/shared_texture_ring_types.cpp"
    "${VOID_NATIVE_DIR}/windows/decode/d3d12va_provider.cpp"
    "${VOID_NATIVE_DIR}/windows/wgpu/d3d12_presentation_backend.cpp"
    "${VOID_NATIVE_DIR}/windows/wgpu/wgpu_d3d12_ffi_bridge.h"
)

if(BUILD_ANALYSIS)
    list(APPEND VOID_RENDERER_WINDOWS_SOURCES
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_primitives.cpp"
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable.cpp")
else()
    list(APPEND VOID_RENDERER_WINDOWS_SOURCES
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable_stub.cpp")
endif()

set(VOID_RENDERER_SOURCES
    ${VOID_RENDERER_CORE_SOURCES}
    ${VOID_RENDERER_WINDOWS_SOURCES}
)
