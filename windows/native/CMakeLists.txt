cmake_minimum_required(VERSION 3.20)
project(video_renderer_native VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()
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

# spdlog (header-only) — try local cache first, fallback to FetchContent
set(SPDLOG_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build/windows/x64/_deps/spdlog-src")
if(EXISTS "${SPDLOG_LOCAL_DIR}/include/spdlog/spdlog.h")
    message(STATUS "Using local spdlog from: ${SPDLOG_LOCAL_DIR}")
    add_library(spdlog_header_only INTERFACE)
    target_include_directories(spdlog_header_only INTERFACE "${SPDLOG_LOCAL_DIR}/include")
    add_library(spdlog::spdlog_header_only ALIAS spdlog_header_only)
else()
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(spdlog)
endif()

# Catch2 v3 — try local cache first, fallback to FetchContent
set(CATCH2_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_deps/catch2-src")
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    if(EXISTS "${CATCH2_LOCAL_DIR}/CMakeLists.txt")
        message(STATUS "Using local Catch2 from: ${CATCH2_LOCAL_DIR}")
        add_subdirectory("${CATCH2_LOCAL_DIR}" "${CMAKE_BINARY_DIR}/_deps/catch2-build")
    else()
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.8.1
            GIT_SHALLOW TRUE
            SOURCE_DIR "${CATCH2_LOCAL_DIR}"
        )
        FetchContent_MakeAvailable(Catch2)
    endif()
endif()

# pybind11 (optional)
option(BUILD_PYTHON "Build Python bindings" ON)
if(BUILD_PYTHON)
    find_package(Python3 COMPONENTS Interpreter Development QUIET)
    find_package(pybind11 CONFIG QUIET)
    if(Python3_FOUND AND pybind11_FOUND)
        message(STATUS "Python: ${Python3_VERSION}")
        message(STATUS "pybind11: ${pybind11_VERSION}")
    else()
        message(STATUS "Python/pybind11 not found, skipping Python bindings")
        set(BUILD_PYTHON OFF)
    endif()
endif()

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

set(VIDEO_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../resources/video")

# ==================== FFmpeg DLLs to output directory ====================

set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")
set(OUTPUT_BIN_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>")

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

enable_testing()
add_subdirectory(video_renderer)
add_subdirectory(analysis)

if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ==================== FFI Shared Library ====================

if(BUILD_FFI OR NOT DEFINED BUILD_FFI)
    add_library(video_renderer_ffi SHARED
        exports/ffi_exports.cpp
    )

    target_include_directories(video_renderer_ffi PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_compile_definitions(video_renderer_ffi PRIVATE
        NAKI_VR_FFI_BUILDING
    )

    target_link_libraries(video_renderer_ffi PRIVATE
        video_renderer_lib
        spdlog::spdlog_header_only
    )

    set_target_properties(video_renderer_ffi PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )

    if(TARGET copy_ffmpeg_dlls)
        add_dependencies(video_renderer_ffi copy_ffmpeg_dlls)
    endif()
endif()

# ==================== Python Module ====================

if(BUILD_PYTHON)
    pybind11_add_module(video_renderer_native
        exports/bindings.cpp
    )

    target_link_libraries(video_renderer_native PRIVATE
        video_renderer_lib
        spdlog::spdlog_header_only
    )

    # Ensure FFmpeg DLLs are available for pybind11 module too
    if(TARGET copy_ffmpeg_dlls)
        add_dependencies(video_renderer_native copy_ffmpeg_dlls)
    endif()
endif()

# ==================== HW Probe Tool ====================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/probe_hw.cpp")
add_executable(probe_hw probe_hw.cpp)
target_link_libraries(probe_hw PRIVATE ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY})
target_include_directories(probe_hw PRIVATE "${FFMPEG_INCLUDE_DIR}")
if(TARGET copy_ffmpeg_dlls)
    add_dependencies(probe_hw copy_ffmpeg_dlls)
endif()
endif()

# ==================== Pipeline Benchmark ====================

add_executable(pipeline_bench
    benchmarks/pipeline_bench.cpp
    benchmarks/bench_demux.cpp
    benchmarks/bench_decode.cpp
    benchmarks/bench_swscale.cpp
    benchmarks/bench_d3d11_upload.cpp
    benchmarks/bench_full_pipeline.cpp
)
target_link_libraries(pipeline_bench PRIVATE
    ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY}
    video_renderer_lib
    spdlog::spdlog_header_only
    d3d11 dxgi d3dcompiler
)
target_include_directories(pipeline_bench PRIVATE
    "${FFMPEG_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
if(TARGET copy_ffmpeg_dlls)
    add_dependencies(pipeline_bench copy_ffmpeg_dlls)
endif()

# ==================== Dist Install ====================

set(DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist")

# Ensure dist directories exist at configure time
file(MAKE_DIRECTORY "${DIST_DIR}/python")
file(MAKE_DIRECTORY "${DIST_DIR}/python/video_renderer")
file(MAKE_DIRECTORY "${DIST_DIR}/ffi")

# Copy FFI artifacts to dist/ffi/
add_custom_command(TARGET video_renderer_ffi POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/ffi"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:video_renderer_ffi>"
        "${DIST_DIR}/ffi/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:video_renderer_ffi>/$<TARGET_FILE_PREFIX:video_renderer_ffi>$<TARGET_FILE_BASE_NAME:video_renderer_ffi>.lib"
        "${DIST_DIR}/ffi/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/exports/ffi_exports.h"
        "${DIST_DIR}/ffi/"
    COMMENT "Installing FFI artifacts to dist/ffi/..."
)

# Copy Python artifacts to dist/python/
if(BUILD_PYTHON)
add_custom_command(TARGET video_renderer_native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/python/video_renderer"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:video_renderer_native>"
        "${DIST_DIR}/python/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/exports/__init__.py"
        "${DIST_DIR}/python/video_renderer/__init__.py"
    COMMENT "Installing Python artifacts to dist/python/..."
)

# Copy FFmpeg DLLs to dist/python/ (needed by the pyd at runtime)
if(EXISTS "${FFMPEG_BIN_DIR}")
    file(GLOB FFMPEG_DLL_FILES "${FFMPEG_BIN_DIR}/*.dll")
    set(DIST_DLL_COPY_CMDS "")
    foreach(DLL ${FFMPEG_DLL_FILES})
        list(APPEND DIST_DLL_COPY_CMDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DLL}" "${DIST_DIR}/python/")
    endforeach()
    add_custom_target(copy_ffmpeg_to_dist ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/python"
        ${DIST_DLL_COPY_CMDS}
        COMMENT "Copying FFmpeg DLLs to dist/python/..."
    )
    add_dependencies(copy_ffmpeg_to_dist copy_ffmpeg_dlls)
endif()
endif() # BUILD_PYTHON

# ==================== Config Summary ====================

message(STATUS "")
message(STATUS "=== Video Renderer Native Build ===")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "  FFmpeg: ${FFMPEG_ROOT}")
message(STATUS "  Test videos: ${VIDEO_TEST_DIR}")
message(STATUS "")
