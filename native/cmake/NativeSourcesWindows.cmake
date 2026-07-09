include_guard(GLOBAL)

set(VOID_RENDERER_WINDOWS_SOURCES
    "${VOID_NATIVE_DIR}/windows/common/windows_crash_handler.cpp"
    "${VOID_NATIVE_DIR}/windows/player/native_player.cpp"
)

if(BUILD_ANALYSIS)
    list(APPEND VOID_RENDERER_WINDOWS_SOURCES
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_primitives.cpp"
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable.cpp")
else()
    list(APPEND VOID_RENDERER_WINDOWS_SOURCES
        "${VOID_NATIVE_DIR}/renderer/overlay/analysis_overlay_renderer_portable_stub.cpp")
endif()

set(VOID_RENDERER_SOURCES
    ${VOID_RENDERER_CORE_SOURCES}
    ${VOID_RENDERER_WINDOWS_SOURCES}
)
