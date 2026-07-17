set(_shader_dir "${VOID_NATIVE_DIR}/macos/metal/shaders")
set(_generated_inc
    "${VOID_NATIVE_DIR}/macos/metal/generated/metal_pixel_buffer_uploader_shaders.inc")
set(_shader_files
    common_types.metal
    common_layout.metal
    common_color.metal
    common_overlay_sampling.metal
    layout_package.metal
    layout_cvpixelbuffer.metal
    common_overlay.metal
    overlay_direct_line.metal
    overlay_layer.metal
    overlay_legacy_composite.metal)

set(_expected "{\n")
foreach(_shader_file IN LISTS _shader_files)
    file(READ "${_shader_dir}/${_shader_file}" _shader_source)
    string(APPEND _expected "R\"(\n${_shader_source})\",\n")
endforeach()
string(APPEND _expected "};\n")
file(READ "${_generated_inc}" _actual)
if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR
        "generated Metal shader include is stale; run "
        "native/macos/metal/generate_metal_shader_inc.py")
endif()
