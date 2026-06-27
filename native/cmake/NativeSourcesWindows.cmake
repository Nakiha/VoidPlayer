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
    "${VOID_NATIVE_DIR}/windows/d3d11/cross_adapter_transport.cpp"
    "${VOID_NATIVE_DIR}/windows/decode/d3d11_frame_snapshot.cpp"
    "${VOID_NATIVE_DIR}/windows/decode/d3d12va_provider.cpp"
    "${VOID_NATIVE_DIR}/windows/wgpu/d3d12_presentation_backend.cpp"
    "${VOID_NATIVE_DIR}/windows/wgpu/wgpu_d3d12_ffi_bridge.h"
)

set(VOID_WINDOWS_ANALYSIS_OVERLAY_D3D11_SOURCE
    "${VOID_NATIVE_DIR}/windows/d3d11/analysis_overlay_renderer.cpp")
set(VOID_WINDOWS_ANALYSIS_OVERLAY_STUB_SOURCE
    "${VOID_NATIVE_DIR}/windows/d3d11/analysis_overlay_renderer_stub.cpp")

if(BUILD_ANALYSIS)
    list(APPEND VOID_RENDERER_WINDOWS_SOURCES
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_primitives.cpp")
endif()

set(VOID_D3D11_BACKEND_SOURCES
    "${VOID_NATIVE_DIR}/windows/d3d11/device.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/fp16_target.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/frame_capture_service.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/frame_presenter.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/headless_output.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/render_backend.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/shared_fp16_ring.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/shared_source_cache_ring.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/texture.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/shader.cpp"
)
if(BUILD_ANALYSIS)
    list(APPEND VOID_D3D11_BACKEND_SOURCES
        ${VOID_WINDOWS_ANALYSIS_OVERLAY_D3D11_SOURCE})
else()
    list(APPEND VOID_D3D11_BACKEND_SOURCES
        ${VOID_WINDOWS_ANALYSIS_OVERLAY_STUB_SOURCE})
endif()

set(VOID_RENDERER_SOURCES
    ${VOID_RENDERER_CORE_SOURCES}
    ${VOID_RENDERER_WINDOWS_SOURCES}
    ${VOID_D3D11_BACKEND_SOURCES}
)
