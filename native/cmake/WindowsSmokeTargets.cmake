include_guard(GLOBAL)

function(void_add_windows_backend_smoke target_name source_file labels)
    add_executable(${target_name} "${VOID_NATIVE_DIR}/${source_file}")
    void_apply_native_compile_options(${target_name})
    target_link_libraries(${target_name} PRIVATE video_renderer_lib)
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES
        TIMEOUT 60
        LABELS "${labels}")
endfunction()
