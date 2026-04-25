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

if(BUILD_PYTHON)
    pybind11_add_module(video_renderer_native
        exports/bindings.cpp
    )

    target_link_libraries(video_renderer_native PRIVATE
        video_renderer_lib
        spdlog::spdlog_header_only
    )

    if(TARGET copy_ffmpeg_dlls)
        add_dependencies(video_renderer_native copy_ffmpeg_dlls)
    endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/probe_hw.cpp")
    add_executable(probe_hw probe_hw.cpp)
    target_link_libraries(probe_hw PRIVATE ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY})
    target_include_directories(probe_hw PRIVATE "${FFMPEG_INCLUDE_DIR}")
    if(TARGET copy_ffmpeg_dlls)
        add_dependencies(probe_hw copy_ffmpeg_dlls)
    endif()
endif()
