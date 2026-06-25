include_guard(GLOBAL)

set(VOID_RENDERER_PORTABLE_OVERLAY_SOURCES
    "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable_stub.cpp")
if(BUILD_ANALYSIS)
    set(VOID_RENDERER_PORTABLE_OVERLAY_SOURCES
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_primitives.cpp"
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable.cpp")
endif()

add_library(void_renderer_portable_driver OBJECT
    ${VOID_RENDERER_PORTABLE_DRIVER_SOURCES}
    ${VOID_RENDERER_PORTABLE_OVERLAY_SOURCES}
)
void_apply_native_compile_options(void_renderer_portable_driver)
target_compile_definitions(void_renderer_portable_driver PRIVATE
    VOID_BUILD_ANALYSIS=$<BOOL:${BUILD_ANALYSIS}>
)
target_link_libraries(void_renderer_portable_driver PRIVATE
    void_media_ffmpeg
)

add_library(void_macos_native_player STATIC
    ${VOID_MACOS_NATIVE_PLAYER_SOURCES}
    $<TARGET_OBJECTS:void_renderer_portable_driver>
)
void_apply_native_compile_options(void_macos_native_player)
target_include_directories(void_macos_native_player PUBLIC
    "${VOID_NATIVE_DIR}/macos"
)
target_compile_definitions(void_macos_native_player PRIVATE
    VOID_BUILD_ANALYSIS=$<BOOL:${BUILD_ANALYSIS}>
)
target_link_libraries(void_macos_native_player PUBLIC
    void_media_ffmpeg
    "-framework Foundation"
    "-framework Metal"
    "-framework CoreVideo"
)

set(VOID_WGPU_RUST_DIR "${VOID_NATIVE_DIR}/rust")
set(VOID_CARGO_HINTS)
if(DEFINED ENV{HOME})
    list(APPEND VOID_CARGO_HINTS "$ENV{HOME}/.cargo/bin")
endif()
find_program(VOID_CARGO_EXECUTABLE cargo HINTS ${VOID_CARGO_HINTS})
if(VOID_CARGO_EXECUTABLE AND EXISTS "${VOID_WGPU_RUST_DIR}/Cargo.toml")
    set(VOID_WGPU_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust")
    set(VOID_WGPU_RUST_LIB
        "${VOID_WGPU_RUST_TARGET_DIR}/$<IF:$<CONFIG:Debug>,debug,release>/libvoidplayer_wgpu_ffi.a")
    set(VOID_WGPU_CARGO_PROFILE_ARGS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND VOID_WGPU_CARGO_PROFILE_ARGS --release)
    endif()
    add_custom_target(voidplayer_wgpu_ffi_rust
        COMMAND "${VOID_CARGO_EXECUTABLE}" build
                ${VOID_WGPU_CARGO_PROFILE_ARGS}
                --manifest-path "${VOID_WGPU_RUST_DIR}/Cargo.toml"
                --target-dir "${VOID_WGPU_RUST_TARGET_DIR}"
        WORKING_DIRECTORY "${VOID_WGPU_RUST_DIR}"
        COMMENT "Building VoidPlayer wgpu Rust FFI"
        VERBATIM)
    add_dependencies(void_macos_native_player voidplayer_wgpu_ffi_rust)
    target_compile_definitions(void_macos_native_player PRIVATE
        VOIDPLAYER_WGPU_RUST_LINKED=1
    )
    target_link_libraries(void_macos_native_player PUBLIC "${VOID_WGPU_RUST_LIB}")
else()
    message(STATUS "VoidPlayer wgpu Rust FFI disabled: cargo not found or native/rust missing")
endif()

target_sources(void_media_ffmpeg PRIVATE
    "${VOID_NATIVE_DIR}/macos/decode/videotoolbox_provider.cpp"
)
if(BUILD_ANALYSIS)
    target_link_libraries(void_macos_native_player PUBLIC analysis_lib)
endif()
target_compile_options(void_macos_native_player PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)

if(BUILD_ANALYSIS)
    include("${CMAKE_CURRENT_LIST_DIR}/AnalysisCliTarget.cmake")
    void_add_analysis_cli(VoidPlayerCli)
endif()
