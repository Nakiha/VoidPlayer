include_guard(GLOBAL)

get_filename_component(VOID_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(VOID_MINIAUDIO_INCLUDE_DIR "${VOID_NATIVE_DIR}/../third_party/miniaudio/include")

option(BUILD_ANALYSIS "Build native analysis cache, CLI, and overlay feature" ON)

include("${CMAKE_CURRENT_LIST_DIR}/NativeSourcesPortable.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/NativeSourcesAnalysis.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/NativeSourcesWindows.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/NativeSourcesMacOS.cmake")

function(void_apply_native_compile_options target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_17)
    if(WIN32)
        target_compile_definitions(${target_name} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()
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
