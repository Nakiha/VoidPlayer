include_guard(GLOBAL)

if(POLICY CMP0109)
    cmake_policy(SET CMP0109 NEW)
endif()

set(VOID_WGPU_RUST_DIR "${VOID_NATIVE_DIR}/rust")
set(VOID_WGPU_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(VOID_CARGO_HINTS)
if(DEFINED ENV{USERPROFILE})
    list(APPEND VOID_CARGO_HINTS "$ENV{USERPROFILE}/.cargo/bin")
endif()
if(DEFINED ENV{HOME})
    list(APPEND VOID_CARGO_HINTS "$ENV{HOME}/.cargo/bin")
endif()
find_program(VOID_CARGO_EXECUTABLE cargo HINTS ${VOID_CARGO_HINTS})
set(VOID_CARGO_COMMAND "${VOID_CARGO_EXECUTABLE}")
set(VOID_RUSTC_EXECUTABLE "")
if(WIN32 AND VOID_CARGO_EXECUTABLE)
    execute_process(
        COMMAND "${VOID_CARGO_EXECUTABLE}" --version
        RESULT_VARIABLE VOID_CARGO_PROBE_RESULT
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT VOID_CARGO_PROBE_RESULT EQUAL 0)
        find_program(VOID_RUSTUP_EXECUTABLE rustup HINTS ${VOID_CARGO_HINTS})
        if(VOID_RUSTUP_EXECUTABLE)
            execute_process(
                COMMAND "${VOID_RUSTUP_EXECUTABLE}" run stable cargo --version
                RESULT_VARIABLE VOID_RUSTUP_CARGO_PROBE_RESULT
                OUTPUT_QUIET
                ERROR_QUIET)
            if(VOID_RUSTUP_CARGO_PROBE_RESULT EQUAL 0)
                set(VOID_CARGO_EXECUTABLE "${VOID_RUSTUP_EXECUTABLE}")
                set(VOID_CARGO_COMMAND "${VOID_RUSTUP_EXECUTABLE};run;stable;cargo")
                execute_process(
                    COMMAND "${VOID_RUSTUP_EXECUTABLE}" which rustc
                    RESULT_VARIABLE VOID_RUSTUP_RUSTC_RESULT
                    OUTPUT_VARIABLE VOID_RUSTUP_RUSTC_PATH
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
                if(VOID_RUSTUP_RUSTC_RESULT EQUAL 0)
                    set(VOID_RUSTC_EXECUTABLE "${VOID_RUSTUP_RUSTC_PATH}")
                endif()
            endif()
        endif()
    endif()
endif()

function(void_link_wgpu_rust_ffi target_name)
    if(NOT VOID_CARGO_COMMAND OR NOT EXISTS "${VOID_WGPU_RUST_DIR}/Cargo.toml")
        message(FATAL_ERROR
            "VoidPlayer wgpu Rust FFI is required for ${target_name}, but cargo "
            "or native/rust/Cargo.toml was not found. Install Rust/Cargo or "
            "bootstrap the pinned toolchain before configuring.")
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
            COMMAND "${CMAKE_COMMAND}"
                    "-DVOID_CARGO_COMMAND:STRING=${VOID_CARGO_COMMAND}"
                    "-DVOID_RUSTC_EXECUTABLE:FILEPATH=${VOID_RUSTC_EXECUTABLE}"
                    "-DVOID_WGPU_RUST_DIR:PATH=${VOID_WGPU_RUST_DIR}"
                    "-DVOID_WGPU_RUST_TARGET_DIR:PATH=${VOID_WGPU_RUST_TARGET_DIR}"
                    "-DVOID_WGPU_RUST_CONFIG=$<CONFIG>"
                    -P "${VOID_WGPU_CMAKE_DIR}/BuildWgpuRust.cmake"
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
