include_guard(GLOBAL)

set(VOID_WGPU_RUST_DIR "${VOID_NATIVE_DIR}/rust")
set(VOID_CARGO_HINTS)
if(DEFINED ENV{USERPROFILE})
    list(APPEND VOID_CARGO_HINTS "$ENV{USERPROFILE}/.cargo/bin")
endif()
if(DEFINED ENV{HOME})
    list(APPEND VOID_CARGO_HINTS "$ENV{HOME}/.cargo/bin")
endif()
find_program(VOID_CARGO_EXECUTABLE cargo HINTS ${VOID_CARGO_HINTS})

function(void_link_wgpu_rust_ffi target_name)
    if(NOT VOID_CARGO_EXECUTABLE OR NOT EXISTS "${VOID_WGPU_RUST_DIR}/Cargo.toml")
        message(STATUS "VoidPlayer wgpu Rust FFI disabled: cargo not found or native/rust missing")
        return()
    endif()

    set(VOID_WGPU_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust")
    if(WIN32)
        set(VOID_WGPU_RUST_LIB
            "${VOID_WGPU_RUST_TARGET_DIR}/$<IF:$<CONFIG:Debug>,debug,release>/voidplayer_wgpu_ffi.lib")
        set(VOID_WGPU_RUST_TARGET_NAME voidplayer_wgpu_ffi_rust_windows)
    elseif(APPLE)
        set(VOID_WGPU_RUST_LIB
            "${VOID_WGPU_RUST_TARGET_DIR}/$<IF:$<CONFIG:Debug>,debug,release>/libvoidplayer_wgpu_ffi.a")
        set(VOID_WGPU_RUST_TARGET_NAME voidplayer_wgpu_ffi_rust_macos)
    else()
        return()
    endif()

    if(NOT TARGET ${VOID_WGPU_RUST_TARGET_NAME})
        add_custom_target(${VOID_WGPU_RUST_TARGET_NAME}
            COMMAND ${CMAKE_COMMAND} -E env
                    RUSTUP_TOOLCHAIN=stable
                    RUSTUP_DIST_SERVER=
                    RUSTUP_UPDATE_ROOT=
                    "${VOID_CARGO_EXECUTABLE}" build
                    "$<$<NOT:$<CONFIG:Debug>>:--release>"
                    --manifest-path "${VOID_WGPU_RUST_DIR}/Cargo.toml"
                    --target-dir "${VOID_WGPU_RUST_TARGET_DIR}"
            WORKING_DIRECTORY "${VOID_WGPU_RUST_DIR}"
            COMMENT "Building VoidPlayer wgpu Rust FFI"
            VERBATIM)
    endif()

    add_dependencies(${target_name} ${VOID_WGPU_RUST_TARGET_NAME})
    target_compile_definitions(${target_name} PRIVATE
        VOIDPLAYER_WGPU_RUST_LINKED=1
    )
    target_link_libraries(${target_name} PUBLIC "${VOID_WGPU_RUST_LIB}")
    if(WIN32)
        target_link_libraries(${target_name} PUBLIC
            bcrypt
            ntdll
            userenv
            ws2_32
        )
    endif()
endfunction()
