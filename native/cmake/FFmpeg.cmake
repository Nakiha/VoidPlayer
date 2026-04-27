include_guard(GLOBAL)

if(NOT DEFINED VOID_NATIVE_DIR)
    get_filename_component(VOID_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if(NOT DEFINED VOID_FFMPEG_REQUIRED)
    set(VOID_FFMPEG_REQUIRED ON)
endif()

if(WIN32)
    set(_VOID_DEFAULT_FFMPEG_ROOT "${VOID_NATIVE_DIR}/../windows/libs/ffmpeg")
else()
    set(_VOID_DEFAULT_FFMPEG_ROOT "${VOID_NATIVE_DIR}/../third_party/ffmpeg")
endif()

set(FFMPEG_ROOT "${_VOID_DEFAULT_FFMPEG_ROOT}" CACHE PATH "FFmpeg root directory")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")
set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")
set(FFMPEG_RUNTIME_DLL_PATTERNS
    "avcodec-*.dll"
    "avformat-*.dll"
    "avutil-*.dll"
    "swresample-*.dll"
    "swscale-*.dll"
)

if(NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libavcodec/avcodec.h")
    if(VOID_FFMPEG_REQUIRED)
        message(FATAL_ERROR "FFmpeg headers not found at ${FFMPEG_INCLUDE_DIR}")
    else()
        message(WARNING "FFmpeg headers not found at ${FFMPEG_INCLUDE_DIR} - video renderer will not be built")
        set(FFMPEG_FOUND FALSE)
        return()
    endif()
endif()

find_library(AVCODEC_LIBRARY avcodec PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(AVFORMAT_LIBRARY avformat PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(AVUTIL_LIBRARY avutil PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(SWSCALE_LIBRARY swscale PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)

set(FFMPEG_FOUND TRUE)
message(STATUS "FFmpeg: avcodec=${AVCODEC_LIBRARY}, avformat=${AVFORMAT_LIBRARY}")

function(void_collect_ffmpeg_runtime_dlls out_var)
    set(_dlls "")
    if(EXISTS "${FFMPEG_BIN_DIR}")
        foreach(_pattern ${FFMPEG_RUNTIME_DLL_PATTERNS})
            file(GLOB _matches "${FFMPEG_BIN_DIR}/${_pattern}")
            list(APPEND _dlls ${_matches})
        endforeach()
        list(REMOVE_DUPLICATES _dlls)
    endif()
    set(${out_var} "${_dlls}" PARENT_SCOPE)
endfunction()
