add_executable(pipeline_bench
    pipeline_bench.cpp
    bench_demux.cpp
    bench_decode.cpp
    bench_swscale.cpp
    bench_d3d11_upload.cpp
    bench_full_pipeline.cpp
)

target_link_libraries(pipeline_bench PRIVATE
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWSCALE_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    video_renderer_lib
    spdlog::spdlog_header_only
    d3d11
    dxgi
    d3dcompiler
)

target_include_directories(pipeline_bench PRIVATE
    "${FFMPEG_INCLUDE_DIR}"
    "${VOID_NATIVE_DIR}"
)

set_target_properties(pipeline_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

if(TARGET copy_ffmpeg_dlls)
    add_dependencies(pipeline_bench copy_ffmpeg_dlls)
endif()
