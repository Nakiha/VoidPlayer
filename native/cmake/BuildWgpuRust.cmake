if(NOT DEFINED VOID_CARGO_COMMAND OR "${VOID_CARGO_COMMAND}" STREQUAL "")
    message(FATAL_ERROR "VOID_CARGO_COMMAND is missing")
endif()
if(NOT DEFINED VOID_WGPU_RUST_DIR OR NOT EXISTS "${VOID_WGPU_RUST_DIR}/Cargo.toml")
    message(FATAL_ERROR "VOID_WGPU_RUST_DIR is missing Cargo.toml")
endif()
if(NOT DEFINED VOID_WGPU_RUST_TARGET_DIR)
    message(FATAL_ERROR "VOID_WGPU_RUST_TARGET_DIR is missing")
endif()

set(_cargo_args
    build
    --manifest-path "${VOID_WGPU_RUST_DIR}/Cargo.toml"
    --target-dir "${VOID_WGPU_RUST_TARGET_DIR}"
)

if(NOT "${VOID_WGPU_RUST_CONFIG}" STREQUAL "Debug")
    list(APPEND _cargo_args --release)
endif()

set(_cargo_env
    RUSTUP_TOOLCHAIN=stable
    RUSTUP_DIST_SERVER=
    RUSTUP_UPDATE_ROOT=
)
if(DEFINED VOID_RUSTC_EXECUTABLE AND NOT "${VOID_RUSTC_EXECUTABLE}" STREQUAL "")
    list(APPEND _cargo_env "RUSTC=${VOID_RUSTC_EXECUTABLE}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            ${_cargo_env}
            ${VOID_CARGO_COMMAND} ${_cargo_args}
    WORKING_DIRECTORY "${VOID_WGPU_RUST_DIR}"
    RESULT_VARIABLE _cargo_result
)
if(NOT _cargo_result EQUAL 0)
    message(FATAL_ERROR
        "VoidPlayer wgpu Rust FFI cargo build failed with ${_cargo_result}")
endif()
