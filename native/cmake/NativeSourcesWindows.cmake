include_guard(GLOBAL)

set(VOID_RENDERER_WINDOWS_SOURCES
    "${VOID_NATIVE_DIR}/windows/common/windows_crash_handler.cpp"
    "${VOID_NATIVE_DIR}/windows/player/native_player.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/capture/frame_capture_service.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/renderer.cpp"
    "${VOID_NATIVE_DIR}/video_renderer/decode/hw/d3d11va_provider.cpp"
)

if(BUILD_ANALYSIS)
    list(APPEND VOID_RENDERER_WINDOWS_SOURCES
        "${VOID_NATIVE_DIR}/video_renderer/overlay/analysis_overlay_primitives.cpp"
        "${VOID_NATIVE_DIR}/video_renderer/overlay/analysis_overlay_renderer.cpp")
else()
    list(APPEND VOID_RENDERER_WINDOWS_SOURCES
        "${VOID_NATIVE_DIR}/video_renderer/overlay/analysis_overlay_renderer_stub.cpp")
endif()

set(VOID_D3D11_BACKEND_SOURCES
    "${VOID_NATIVE_DIR}/windows/d3d11/device.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/frame_presenter.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/headless_output.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/render_backend.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/texture.cpp"
    "${VOID_NATIVE_DIR}/windows/d3d11/shader.cpp"
)

set(VOID_RENDERER_SOURCES
    ${VOID_RENDERER_CORE_SOURCES}
    ${VOID_RENDERER_WINDOWS_SOURCES}
    ${VOID_D3D11_BACKEND_SOURCES}
)
