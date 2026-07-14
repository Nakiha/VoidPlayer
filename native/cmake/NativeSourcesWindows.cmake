include_guard(GLOBAL)

set(VOID_RENDERER_WINDOWS_OVERLAY_SOURCES
    "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable_stub.cpp")
if(BUILD_ANALYSIS)
    set(VOID_RENDERER_WINDOWS_OVERLAY_SOURCES
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_primitives.cpp"
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable.cpp")
endif()

set(VOID_RENDERER_WINDOWS_SOURCES
    ${VOID_RENDERER_WINDOWS_OVERLAY_SOURCES}
    "${VOID_NATIVE_DIR}/windows/decode/d3d11_frame_snapshot.cpp"
    "${VOID_NATIVE_DIR}/windows/decode/d3d11va_provider.cpp"
    "${VOID_NATIVE_DIR}/windows/player/native_player.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_d3d11_target_ring.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_d3d11_viewport_renderer.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_display_resolver.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_presentation_backend.cpp"
    "${VOID_NATIVE_DIR}/windows/presentation/windows_presentation_policy.cpp"
)

set(VOID_RENDERER_SOURCES
    ${VOID_RENDERER_CORE_SOURCES}
    ${VOID_RENDERER_WINDOWS_SOURCES}
)
