include_guard(GLOBAL)

set(VOID_MACOS_NATIVE_PLAYER_SOURCES
    "${VOID_NATIVE_DIR}/macos/macos_crash_handler.cpp"
    "${VOID_NATIVE_DIR}/macos/metal_layout_params.cpp"
    "${VOID_NATIVE_DIR}/macos/metal_presentation_backend_bridge.h"
    "${VOID_NATIVE_DIR}/macos/metal_presentation_backend_bridge.cpp"
    "${VOID_NATIVE_DIR}/macos/metal_presentation_backend.cpp"
    "${VOID_NATIVE_DIR}/macos/metal_uploader_bridge.h"
    "${VOID_NATIVE_DIR}/macos/metal_uploader_bridge.mm"
    "${VOID_NATIVE_DIR}/macos/metal_uploader_internal.h"
    "${VOID_NATIVE_DIR}/macos/metal_texture_wrapping.mm"
    "${VOID_NATIVE_DIR}/macos/native_player_bridge.cpp"
    "${VOID_NATIVE_DIR}/macos/native_player_types.h"
    "${VOID_NATIVE_DIR}/macos/native_player_diagnostics.cpp"
    "${VOID_NATIVE_DIR}/macos/native_player_layout.cpp"
    "${VOID_NATIVE_DIR}/macos/native_player_presentation_target.cpp"
    "${VOID_NATIVE_DIR}/macos/native_player_state.cpp"
    "${VOID_NATIVE_DIR}/macos/native_player_transport.cpp"
    "${VOID_NATIVE_DIR}/macos/metal_pixel_buffer_uploader.mm"
    "${VOID_NATIVE_DIR}/macos/presentation_adapter.cpp"
    "${VOID_NATIVE_DIR}/macos/presentation_cv_pixel_buffer_frame.cpp"
    "${VOID_NATIVE_DIR}/macos/presentation_package_builder.cpp"
)

if(BUILD_ANALYSIS)
    list(APPEND VOID_MACOS_NATIVE_PLAYER_SOURCES
        "${VOID_NATIVE_DIR}/macos/analysis_ffi_bridge.cpp")
endif()
