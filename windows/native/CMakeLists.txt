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

enable_testing()
add_subdirectory(video_renderer)
add_subdirectory(analysis)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

include(cmake/NativeTargets.cmake)
include(cmake/NativeInstall.cmake)

message(STATUS "")
message(STATUS "=== VoidPlayer Native Build ===")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "  FFmpeg: ${FFMPEG_ROOT}")
message(STATUS "  Test videos: ${VIDEO_TEST_DIR}")
message(STATUS "")
