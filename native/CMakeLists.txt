cmake_minimum_required(VERSION 3.16)
project(voidview_native VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Windows 特定设置
if(WIN32)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

# ==================== FFmpeg 配置 ====================

# FFmpeg 根目录
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../libs/ffmpeg" CACHE PATH "FFmpeg root directory")

# FFmpeg 头文件
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")

# FFmpeg 库文件
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")

# 检查 FFmpeg 是否存在
if(NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg headers not found at ${FFMPEG_INCLUDE_DIR}")
endif()

# 查找 FFmpeg 库
find_library(AVCODEC_LIBRARY avcodec PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(AVFORMAT_LIBRARY avformat PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(AVUTIL_LIBRARY avutil PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(SWSCALE_LIBRARY swscale PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)

message(STATUS "FFmpeg found:")
message(STATUS "  avcodec: ${AVCODEC_LIBRARY}")
message(STATUS "  avformat: ${AVFORMAT_LIBRARY}")
message(STATUS "  avutil: ${AVUTIL_LIBRARY}")

# ==================== spdlog 配置 (header-only) ====================

include(FetchContent)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

message(STATUS "spdlog: header-only mode")

# ==================== pybind11 配置 ====================

find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

message(STATUS "Python: ${Python3_VERSION}")
message(STATUS "pybind11: ${pybind11_VERSION}")

# ==================== 编译时间 ====================

# 获取编译时间
string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

# ==================== 源文件 ====================

set(VOIDVIEW_SOURCES
    src/bindings.cpp
    src/hardware_decoder.cpp
    src/texture_interop.cpp
    src/frame_queue.cpp
    src/media_probe.cpp
)

set(VOIDVIEW_HEADERS
    include/voidview_native/hardware_decoder.hpp
    include/voidview_native/texture_interop.hpp
    include/voidview_native/frame_queue.hpp
    include/voidview_native/media_probe.hpp
    include/voidview_native/logger.hpp
    include/voidview_native/version.hpp
)

# ==================== 源文件组 ====================

source_group("Sources" FILES ${VOIDVIEW_SOURCES})
source_group("Headers" FILES ${VOIDVIEW_HEADERS})

# ==================== 创建 Python 模块 ====================

pybind11_add_module(voidview_native ${VOIDVIEW_SOURCES} ${VOIDVIEW_HEADERS})

# 编译时间定义
target_compile_definitions(voidview_native PRIVATE
    VOIDVIEW_BUILD_TIME="${BUILD_TIME}"
)

# 包含目录
target_include_directories(voidview_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${FFMPEG_INCLUDE_DIR}
)

# 链接 spdlog (header-only)
target_link_libraries(voidview_native PRIVATE spdlog::spdlog_header_only)

# 链接 FFmpeg
target_link_libraries(voidview_native PRIVATE
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${SWSCALE_LIBRARY}
)

# Windows 特定链接
if(WIN32)
    # D3D11 和 OpenGL
    target_link_libraries(voidview_native PRIVATE
        dxgi
        d3d11
        opengl32
    )

    # 复制 FFmpeg DLL 到输出目录
    set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")
    if(EXISTS "${FFMPEG_BIN_DIR}")
        add_custom_command(TARGET voidview_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${FFMPEG_BIN_DIR}"
                "$<TARGET_FILE_DIR:voidview_native>"
            COMMENT "Copying FFmpeg DLLs..."
        )
    endif()

    # 复制产物到 player 目录 (重命名为标准名称)
    add_custom_command(TARGET voidview_native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:voidview_native>"
            "${CMAKE_CURRENT_SOURCE_DIR}/../player/voidview_native.pyd"
        COMMENT "Copying voidview_native.pyd to player directory..."
    )
endif()

# ==================== 安装 ====================

install(TARGETS voidview_native LIBRARY DESTINATION .)

# ==================== 打印配置信息 ====================

message(STATUS "")
message(STATUS "=== VoidView Native Build Configuration ===")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "  FFmpeg Root: ${FFMPEG_ROOT}")
message(STATUS "  Output: ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}")
message(STATUS "")
