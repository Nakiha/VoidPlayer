cmake_minimum_required(VERSION 3.20)
project(video_renderer_native VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Windows specific
if(WIN32)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# ==================== Dependencies ====================

include(FetchContent)

# spdlog (header-only)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

# Catch2 v3
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

# pybind11
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

message(STATUS "Python: ${Python3_VERSION}")
message(STATUS "pybind11: ${pybind11_VERSION}")

# ==================== FFmpeg ====================

set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libs/ffmpeg" CACHE PATH "FFmpeg root directory")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")

if(NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg headers not found at ${FFMPEG_INCLUDE_DIR}")
endif()

find_library(AVCODEC_LIBRARY avcodec PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(AVFORMAT_LIBRARY avformat PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(AVUTIL_LIBRARY avutil PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(SWSCALE_LIBRARY swscale PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)

message(STATUS "FFmpeg: avcodec=${AVCODEC_LIBRARY}, avformat=${AVFORMAT_LIBRARY}")

# ==================== Test video path ====================

set(VIDEO_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../resources/video")

# ==================== FFmpeg DLLs to output directory ====================

set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")
set(OUTPUT_BIN_DIR "${CMAKE_BINARY_DIR}/Release")

# Copy FFmpeg DLLs early via a custom target so they're available
# before catch_discover_tests runs test exe at build time
if(EXISTS "${FFMPEG_BIN_DIR}")
    file(GLOB FFMPEG_DLL_FILES "${FFMPEG_BIN_DIR}/*.dll")
    set(FFMPEG_DLL_COPY_CMDS "")
    foreach(DLL ${FFMPEG_DLL_FILES})
        list(APPEND FFMPEG_DLL_COPY_CMDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "${OUTPUT_BIN_DIR}/")
    endforeach()
    add_custom_target(copy_ffmpeg_dlls ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OUTPUT_BIN_DIR}"
        ${FFMPEG_DLL_COPY_CMDS}
        COMMENT "Copying FFmpeg DLLs to output directory..."
    )
endif()

# ==================== Subdirectories ====================

add_subdirectory(video_renderer)
add_subdirectory(tests)

# ==================== Python Module ====================

pybind11_add_module(video_renderer_native
    bindings/bindings.cpp
)

target_link_libraries(video_renderer_native PRIVATE
    video_renderer_lib
    spdlog::spdlog_header_only
)

# Ensure FFmpeg DLLs are available for pybind11 module too
if(TARGET copy_ffmpeg_dlls)
    add_dependencies(video_renderer_native copy_ffmpeg_dlls)
endif()

# ==================== HW Probe Tool ====================

add_executable(probe_hw
    probe_hw.cpp
)
target_link_libraries(probe_hw PRIVATE
    ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY}
)
target_include_directories(probe_hw PRIVATE
    "${FFMPEG_INCLUDE_DIR}"
)
if(EXISTS "${FFMPEG_BIN_DIR}")
    add_custom_command(TARGET probe_hw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FFMPEG_BIN_DIR}"
            "$<TARGET_FILE_DIR:probe_hw>"
        COMMENT "Copying FFmpeg DLLs for probe_hw..."
    )
endif()

# ==================== Pipeline Benchmark ====================

add_executable(pipeline_bench
    benchmarks/pipeline_bench.cpp
)
target_link_libraries(pipeline_bench PRIVATE
    ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY}
    spdlog::spdlog_header_only
    d3d11 dxgi d3dcompiler
)
target_include_directories(pipeline_bench PRIVATE
    "${FFMPEG_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
# Copy FFmpeg DLLs
if(EXISTS "${FFMPEG_BIN_DIR}")
    add_custom_command(TARGET pipeline_bench POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FFMPEG_BIN_DIR}"
            "$<TARGET_FILE_DIR:pipeline_bench>"
        COMMENT "Copying FFmpeg DLLs for benchmark..."
    )
endif()

# ==================== Config Summary ====================

message(STATUS "")
message(STATUS "=== Video Renderer Native Build ===")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "  FFmpeg: ${FFMPEG_ROOT}")
message(STATUS "  Test videos: ${VIDEO_TEST_DIR}")
message(STATUS "")
