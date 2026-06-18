include_guard(GLOBAL)

if(NOT DEFINED VOID_NATIVE_DIR)
    get_filename_component(VOID_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if(NOT DEFINED VOID_FFMPEG_REQUIRED)
    set(VOID_FFMPEG_REQUIRED ON)
endif()

if(WIN32)
    set(_VOID_DEFAULT_FFMPEG_ROOT "${VOID_NATIVE_DIR}/../.toolchains/ffmpeg/windows-x64")
else()
    set(_VOID_DEFAULT_FFMPEG_ROOT "${VOID_NATIVE_DIR}/../.toolchains/ffmpeg/macos-arm64")
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

foreach(_VOID_FFMPEG_LIBRARY_CACHE
        AVCODEC_LIBRARY
        AVFORMAT_LIBRARY
        AVUTIL_LIBRARY
        SWRESAMPLE_LIBRARY
        SWSCALE_LIBRARY)
    unset(${_VOID_FFMPEG_LIBRARY_CACHE} CACHE)
endforeach()

find_library(AVCODEC_LIBRARY avcodec PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(AVFORMAT_LIBRARY avformat PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(AVUTIL_LIBRARY avutil PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_library(SWSCALE_LIBRARY swscale PATHS ${FFMPEG_LIB_DIR} NO_DEFAULT_PATH)

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

function(void_collect_ffmpeg_notice_files out_var)
    set(_notices "")
    foreach(_name README.txt LICENSE LICENSE.txt)
        if(EXISTS "${FFMPEG_ROOT}/${_name}")
            list(APPEND _notices "${FFMPEG_ROOT}/${_name}")
        endif()
    endforeach()
    set(${out_var} "${_notices}" PARENT_SCOPE)
endfunction()

function(void_collect_ffmpeg_notice_dirs out_var)
    set(_notice_dirs "")
    foreach(_name LICENSES)
        if(IS_DIRECTORY "${FFMPEG_ROOT}/${_name}")
            list(APPEND _notice_dirs "${FFMPEG_ROOT}/${_name}")
        endif()
    endforeach()
    set(${out_var} "${_notice_dirs}" PARENT_SCOPE)
endfunction()
