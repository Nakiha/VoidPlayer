include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/NativeSources.cmake")

function(void_add_analysis_cli target_name)
    if(NOT BUILD_ANALYSIS)
        message(FATAL_ERROR "void_add_analysis_cli requires BUILD_ANALYSIS=ON")
    endif()
    if(NOT TARGET analysis_lib)
        message(FATAL_ERROR "void_add_analysis_cli requires analysis_lib")
    endif()

    set(_void_analysis_overlay_benchmark
        "${VOID_NATIVE_DIR}/tools/analysis_overlay_gpu_benchmark_stub.cpp")
    if(WIN32)
        set(_void_analysis_overlay_benchmark
            "${VOID_NATIVE_DIR}/tools/analysis_overlay_gpu_benchmark.cpp")
    endif()

    add_executable(${target_name}
        "${VOID_NATIVE_DIR}/tools/void_player_cli.cpp"
        "${_void_analysis_overlay_benchmark}"
    )
    void_apply_native_compile_options(${target_name})
    target_include_directories(${target_name} PRIVATE
        "${VOID_NATIVE_DIR}"
        "${CMAKE_BINARY_DIR}/renderer"
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(${target_name} PRIVATE analysis_lib)
    if(WIN32)
        target_link_libraries(${target_name} PRIVATE
            d3d11
            dxgi
            d3dcompiler)
    endif()
endfunction()
