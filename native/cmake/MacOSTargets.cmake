include_guard(GLOBAL)

set(VOID_RENDERER_PORTABLE_OVERLAY_SOURCES
    "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable_stub.cpp")
if(BUILD_ANALYSIS)
    set(VOID_RENDERER_PORTABLE_OVERLAY_SOURCES
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_gpu_geometry.cpp"
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_primitives.cpp"
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable.cpp")
endif()

add_library(void_renderer_portable_driver OBJECT
    ${VOID_RENDERER_PORTABLE_DRIVER_SOURCES}
    ${VOID_RENDERER_PORTABLE_OVERLAY_SOURCES}
)
void_apply_native_compile_options(void_renderer_portable_driver)
target_compile_definitions(void_renderer_portable_driver PRIVATE
    VOID_BUILD_ANALYSIS=$<BOOL:${BUILD_ANALYSIS}>
)
target_link_libraries(void_renderer_portable_driver PRIVATE
    void_media_ffmpeg
)

add_library(void_macos_native_player STATIC
    ${VOID_MACOS_NATIVE_PLAYER_SOURCES}
    $<TARGET_OBJECTS:void_renderer_portable_driver>
)
void_apply_native_compile_options(void_macos_native_player)
target_include_directories(void_macos_native_player PUBLIC
    "${VOID_NATIVE_DIR}/macos"
)
target_compile_definitions(void_macos_native_player PRIVATE
    VOID_BUILD_ANALYSIS=$<BOOL:${BUILD_ANALYSIS}>
    VOIDPLAYER_METAL_RUNTIME_SHADER_FALLBACK=$<BOOL:${BUILD_TESTS}>
)
target_link_libraries(void_macos_native_player PUBLIC
    void_media_ffmpeg
    "-framework Foundation"
    "-framework Metal"
    "-framework CoreVideo"
)

if(BUILD_ANALYSIS)
    target_link_libraries(void_macos_native_player PUBLIC analysis_lib)
endif()
target_compile_options(void_macos_native_player PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)

if(BUILD_ANALYSIS)
    include("${CMAKE_CURRENT_LIST_DIR}/AnalysisCliTarget.cmake")
    void_add_analysis_cli(VoidPlayerCli)
endif()
