if(BUILD_FFI)
    add_library(video_renderer_ffi SHARED
        renderer/exports/ffi_exports.cpp
        renderer/exports/ffi_marshalling.cpp
        renderer/exports/ffi_player_commands.cpp
        renderer/exports/ffi_player_lifecycle.cpp
        renderer/exports/ffi_process_globals.cpp
        renderer/exports/ffi_player_registry.cpp
    )
    void_apply_native_compile_options(video_renderer_ffi)

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
        renderer/exports/bindings.cpp
    )
    void_apply_native_compile_options(video_renderer_native)

    target_link_libraries(video_renderer_native PRIVATE
        video_renderer_lib
        spdlog::spdlog_header_only
    )

    if(TARGET copy_ffmpeg_dlls)
        add_dependencies(video_renderer_native copy_ffmpeg_dlls)
    endif()
endif()
