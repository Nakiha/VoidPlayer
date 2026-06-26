include_guard(GLOBAL)

set(VOID_MACOS_NATIVE_PLAYER_SOURCES
    "${VOID_NATIVE_DIR}/macos/common/macos_crash_handler.cpp"
    "${VOID_NATIVE_DIR}/macos/metal/metal_layout_params.cpp"
    "${VOID_NATIVE_DIR}/macos/metal/metal_presentation_backend_bridge.h"
    "${VOID_NATIVE_DIR}/macos/metal/metal_presentation_backend_bridge.cpp"
    "${VOID_NATIVE_DIR}/macos/metal/metal_uploader_bridge.h"
    "${VOID_NATIVE_DIR}/macos/metal/metal_uploader_bridge.mm"
    "${VOID_NATIVE_DIR}/macos/metal/metal_uploader_internal.h"
    "${VOID_NATIVE_DIR}/macos/metal/metal_texture_wrapping.mm"
    "${VOID_NATIVE_DIR}/macos/wgpu/wgpu_ffi_bridge.h"
    "${VOID_NATIVE_DIR}/macos/wgpu/wgpu_ffi_stub.cpp"
    "${VOID_NATIVE_DIR}/macos/wgpu/wgpu_metal_presentation_backend.h"
    "${VOID_NATIVE_DIR}/macos/wgpu/wgpu_metal_presentation_backend.mm"
    "${VOID_NATIVE_DIR}/macos/player/native_player_bridge.cpp"
    "${VOID_NATIVE_DIR}/macos/player/native_player_types.h"
    "${VOID_NATIVE_DIR}/macos/player/native_player_diagnostics.cpp"
    "${VOID_NATIVE_DIR}/macos/player/native_player_layout.cpp"
    "${VOID_NATIVE_DIR}/macos/player/native_player_presentation_target.cpp"
    "${VOID_NATIVE_DIR}/macos/player/native_player_state.cpp"
    "${VOID_NATIVE_DIR}/macos/player/native_player_transport.cpp"
    "${VOID_NATIVE_DIR}/macos/metal/metal_pixel_buffer_uploader.mm"
    "${VOID_NATIVE_DIR}/macos/presentation/presentation_adapter.cpp"
    "${VOID_NATIVE_DIR}/macos/presentation/presentation_cv_pixel_buffer_frame.cpp"
    "${VOID_NATIVE_DIR}/macos/presentation/presentation_package_builder.cpp"
)

if(BUILD_ANALYSIS)
    list(APPEND VOID_MACOS_NATIVE_PLAYER_SOURCES
        "${VOID_NATIVE_DIR}/macos/analysis/analysis_ffi_bridge.cpp")
endif()
