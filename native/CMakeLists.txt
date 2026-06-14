cmake_minimum_required(VERSION 3.20)
project(VoidPlayerNative VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

if(WIN32)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

include(cmake/Dependencies.cmake)
include(cmake/FFmpeg.cmake)
include(cmake/NativeSources.cmake)
include(cmake/TestAssets.cmake)
include(cmake/RuntimeArtifacts.cmake)

option(BUILD_FFI "Build C FFI shared library" ON)
option(VOID_ENABLE_WARNINGS_AS_ERRORS "Treat native compiler warnings as errors" OFF)
set(VOID_SANITIZER "" CACHE STRING "Native sanitizer to enable: address, thread, or undefined")
set_property(CACHE VOID_SANITIZER PROPERTY STRINGS "" "address" "thread" "undefined")

enable_testing()
if(BUILD_ANALYSIS)
    add_subdirectory(analysis)
endif()

if(WIN32)
    add_subdirectory(renderer)
else()
    include(cmake/PortableTargets.cmake)
    message(STATUS "Windows D3D11 renderer targets disabled on this platform")
endif()

if(WIN32 AND BUILD_TESTS)
    add_subdirectory(tests)
    add_executable(windows_d3d11_color_layout_parity_smoke
        tools/windows_d3d11_color_layout_parity_smoke.cpp)
    void_apply_native_compile_options(windows_d3d11_color_layout_parity_smoke)
    target_link_libraries(windows_d3d11_color_layout_parity_smoke PRIVATE
        video_renderer_lib)
    add_test(NAME windows_d3d11_color_layout_parity_smoke
        COMMAND windows_d3d11_color_layout_parity_smoke)
    set_tests_properties(windows_d3d11_color_layout_parity_smoke PROPERTIES
        TIMEOUT 60
        LABELS "windows;backend;contract")

    add_executable(windows_d3d11_fp16_scrgb_smoke
        tools/windows_d3d11_fp16_scrgb_smoke.cpp)
    void_apply_native_compile_options(windows_d3d11_fp16_scrgb_smoke)
    target_link_libraries(windows_d3d11_fp16_scrgb_smoke PRIVATE
        video_renderer_lib)
    add_test(NAME windows_d3d11_fp16_scrgb_smoke
        COMMAND windows_d3d11_fp16_scrgb_smoke)
    set_tests_properties(windows_d3d11_fp16_scrgb_smoke PROPERTIES
        TIMEOUT 60
        LABELS "windows;backend;contract;canary;fp16")
endif()

if(WIN32 AND BUILD_ANALYSIS)
    add_executable(VoidPlayerCli
        tools/void_player_cli.cpp
        tools/analysis_overlay_gpu_benchmark.cpp)
    void_apply_native_compile_options(VoidPlayerCli)
    target_include_directories(VoidPlayerCli PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/renderer")
    target_link_libraries(VoidPlayerCli PRIVATE
        analysis_lib
        d3d11
        dxgi
        d3dcompiler)
    if(BUILD_TESTS)
        add_executable(analysis_cli_smoke_tests
            tests/analysis/test_analysis_cli.cpp
            analysis/cache/overlay_chunk.cpp
            analysis/parsers/vac2_parser.cpp
            analysis/parsers/vachunk_parser.cpp
        )
        void_apply_native_compile_options(analysis_cli_smoke_tests)
        target_include_directories(analysis_cli_smoke_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(analysis_cli_smoke_tests PRIVATE spdlog::spdlog_header_only)
        void_link_zstd(analysis_cli_smoke_tests)
        add_dependencies(analysis_cli_smoke_tests VoidPlayerCli)
        add_test(NAME voidplayer_cli_help COMMAND VoidPlayerCli --help)
        set_tests_properties(voidplayer_cli_help PROPERTIES
            LABELS "windows;analysis;cli;canary")
        add_test(NAME analysis_cli_smoke COMMAND
            analysis_cli_smoke_tests
            $<TARGET_FILE:VoidPlayerCli>
            "${FFMPEG_ANALYZER_PATH}"
            "${VIDEO_TEST_DIR}/h265_10s_1920x1080.mp4")
        set_tests_properties(analysis_cli_smoke PROPERTIES
            LABELS "windows;analysis;cli;integration")
    endif()
endif()

if(WIN32)
    include(cmake/WindowsBindingTargets.cmake)
endif()
include(cmake/NativeInstall.cmake)

message(STATUS "")
message(STATUS "=== VoidPlayer Native Build ===")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "  FFmpeg: ${FFMPEG_ROOT}")
message(STATUS "  Test videos: ${VIDEO_TEST_DIR}")
message(STATUS "  Analysis: ${BUILD_ANALYSIS}")
message(STATUS "")
