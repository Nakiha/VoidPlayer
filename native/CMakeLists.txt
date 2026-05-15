cmake_minimum_required(VERSION 3.20)
project(video_renderer_native VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

option(BUILD_BENCHMARKS "Build native benchmark executables" OFF)
option(BUILD_FFI "Build C FFI shared library" ON)

enable_testing()
if(BUILD_ANALYSIS)
    add_subdirectory(analysis)
endif()

if(WIN32)
    add_subdirectory(video_renderer)
else()
    message(STATUS "video_renderer disabled: D3D11 backend is Windows-only")
endif()

if(WIN32 AND BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(WIN32 AND BUILD_ANALYSIS)
    add_executable(VoidPlayerCli
        tools/void_player_cli.cpp
        tools/analysis_overlay_gpu_benchmark.cpp)
    void_apply_native_compile_options(VoidPlayerCli)
    target_include_directories(VoidPlayerCli PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/video_renderer")
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
        add_test(NAME analysis_cli_smoke COMMAND
            analysis_cli_smoke_tests
            $<TARGET_FILE:VoidPlayerCli>
            "${FFMPEG_ANALYZER_PATH}"
            "${VIDEO_TEST_DIR}/h265_10s_1920x1080.mp4")
    endif()
endif()

if(WIN32)
    include(cmake/NativeTargets.cmake)
endif()
include(cmake/NativeInstall.cmake)

message(STATUS "")
message(STATUS "=== VoidPlayer Native Build ===")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "  FFmpeg: ${FFMPEG_ROOT}")
message(STATUS "  Test videos: ${VIDEO_TEST_DIR}")
message(STATUS "  Analysis: ${BUILD_ANALYSIS}")
message(STATUS "")
