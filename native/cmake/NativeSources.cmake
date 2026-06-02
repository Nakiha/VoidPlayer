include_guard(GLOBAL)

get_filename_component(VOID_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VOID_MINIAUDIO_INCLUDE_DIR "${VOID_NATIVE_DIR}/../third_party/miniaudio/include")

option(BUILD_ANALYSIS "Build native analysis cache, CLI, and overlay feature" ON)

include("${CMAKE_CURRENT_LIST_DIR}/NativeSourcesPortable.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/NativeSourcesAnalysis.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/NativeSourcesWindows.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/NativeSourcesMacOS.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/NativeSourcesShaders.cmake")

function(void_apply_native_compile_options target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /utf-8 /W4 /WX /permissive- /EHsc)
    else()
        target_compile_options(${target_name} PRIVATE
            $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
            $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor>
            $<$<COMPILE_LANG_AND_ID:OBJCXX,AppleClang,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor>
        )
        if(VOID_ENABLE_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE
                $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang,GNU>:-Werror>
                $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Werror>
                $<$<COMPILE_LANG_AND_ID:OBJCXX,AppleClang,Clang>:-Werror>
            )
        endif()
        if(VOID_SANITIZER)
            if(VOID_SANITIZER STREQUAL "address")
                set(_void_sanitizer_flag "-fsanitize=address")
            elseif(VOID_SANITIZER STREQUAL "thread")
                set(_void_sanitizer_flag "-fsanitize=thread")
            elseif(VOID_SANITIZER STREQUAL "undefined")
                set(_void_sanitizer_flag "-fsanitize=undefined")
            else()
                message(FATAL_ERROR "Unsupported VOID_SANITIZER=${VOID_SANITIZER}")
            endif()
            target_compile_options(${target_name} PRIVATE "${_void_sanitizer_flag}" -fno-omit-frame-pointer)
            target_link_options(${target_name} PRIVATE "${_void_sanitizer_flag}")
        endif()
    endif()
endfunction()

function(void_configure_renderer_shaders output_dir)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${VOID_RENDERER_SHADER_SOURCES})
    file(READ "${VOID_NATIVE_DIR}/renderer/shaders/common.hlsl" COMMON_HLSL)
    file(READ "${VOID_NATIVE_DIR}/renderer/shaders/color_pipeline.hlsl" COLOR_PIPELINE_HLSL)
    file(READ "${VOID_NATIVE_DIR}/renderer/shaders/sampling.hlsl" SAMPLING_HLSL)
    file(READ "${VOID_NATIVE_DIR}/renderer/shaders/multitrack.hlsl" MULTITRACK_HLSL)
    file(READ "${VOID_NATIVE_DIR}/renderer/shaders/analysis_overlay.hlsl" ANALYSIS_OVERLAY_HLSL)
    file(READ "${VOID_NATIVE_DIR}/renderer/shaders/analysis_overlay_invert.hlsl" ANALYSIS_OVERLAY_INVERT_HLSL)
    file(READ "${VOID_NATIVE_DIR}/renderer/shaders/analysis_overlay_contrast.hlsl" ANALYSIS_OVERLAY_CONTRAST_HLSL)
    file(READ "${VOID_NATIVE_DIR}/renderer/shaders/analysis_overlay_rect.hlsl" ANALYSIS_OVERLAY_RECT_HLSL)
    file(READ "${VOID_NATIVE_DIR}/renderer/shaders/analysis_overlay_mask_rect.hlsl" ANALYSIS_OVERLAY_MASK_RECT_HLSL)
    configure_file(
        "${VOID_RENDERER_SHADER_TEMPLATE}"
        "${output_dir}/embedded_shaders.h"
        @ONLY
    )
endfunction()
