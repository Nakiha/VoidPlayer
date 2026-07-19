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
include(cmake/AnalysisCliTarget.cmake)
include(cmake/TestAssets.cmake)
include(cmake/RuntimeArtifacts.cmake)
include(cmake/WindowsSmokeTargets.cmake)

option(VOID_ENABLE_WARNINGS_AS_ERRORS "Treat native compiler warnings as errors" OFF)
set(VOID_SANITIZER "" CACHE STRING "Native sanitizer to enable: address, thread, or undefined")
set_property(CACHE VOID_SANITIZER PROPERTY STRINGS "" "address" "thread" "undefined")

enable_testing()
find_package(Threads REQUIRED)
include(cmake/VideoDecodeCoreTarget.cmake)
if(BUILD_ANALYSIS)
    add_subdirectory(analysis)
endif()

if(WIN32)
    add_subdirectory(renderer)
else()
    include(cmake/PortableTargets.cmake)
    message(STATUS "Windows renderer targets disabled on this platform")
endif()

if(WIN32 AND BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(WIN32 AND BUILD_ANALYSIS)
    void_add_analysis_cli(VoidPlayerCli)
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

if(UNIX AND NOT APPLE AND BUILD_ANALYSIS)
    void_add_analysis_cli(VoidPlayerCli)
    if(BUILD_TESTS)
        add_test(NAME voidplayer_cli_help COMMAND VoidPlayerCli --help)
        set_tests_properties(voidplayer_cli_help PROPERTIES
            LABELS "linux;analysis;cli;canary")
    endif()
endif()

if(BUILD_ANALYSIS AND BUILD_TESTS)
    add_executable(quality_metrics_tests
        tests/test_main.cpp
        tests/analysis/test_quality_metrics.cpp
    )
    void_apply_native_compile_options(quality_metrics_tests)
    target_include_directories(quality_metrics_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis")
    target_link_libraries(quality_metrics_tests PRIVATE
        analysis_lib
        Catch2::Catch2
    )
    add_test(NAME quality_metrics_tests COMMAND quality_metrics_tests)
    set_tests_properties(quality_metrics_tests PROPERTIES
        LABELS "analysis;quality;contract")
endif()

message(STATUS "")
message(STATUS "=== VoidPlayer Native Build ===")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "  FFmpeg: ${FFMPEG_ROOT}")
message(STATUS "  Test videos: ${VIDEO_TEST_DIR}")
message(STATUS "  Analysis: ${BUILD_ANALYSIS}")
message(STATUS "  Quality backend: CPU/SIMD")
message(STATUS "")
