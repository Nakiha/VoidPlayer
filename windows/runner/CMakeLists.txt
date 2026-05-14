cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_texture_bridge.cpp"
  "flutter_window.cpp"
  "main.cpp"
  "native_diagnostics_provider.cpp"
  "native_logging_bootstrap.cpp"
  "native_player_registry.cpp"
  "startup_trace.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "viewport_capture_service.cpp"
  "video_renderer_plugin.cpp"
  "analysis_ffi.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})
if(MSVC)
  target_compile_options(${BINARY_NAME} PRIVATE /utf-8)
endif()

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app flutter_wrapper_plugin)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib")
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# ==================== Native Video Renderer ====================

if(FFMPEG_FOUND)
  include("${CMAKE_CURRENT_SOURCE_DIR}/../../native/cmake/FlutterNativeTarget.cmake")
  void_configure_flutter_native_target(${BINARY_NAME} "${CMAKE_CURRENT_BINARY_DIR}")

  add_executable(VoidPlayerCli
    "../../native/tools/void_player_cli.cpp"
    "../../native/tools/analysis_overlay_gpu_benchmark.cpp"
    "../../native/analysis/cache/overlay_chunk.cpp"
    "../../native/analysis/cache/overlay_raster.cpp"
    "../../native/analysis/cache/vacache_store.cpp"
    "../../native/analysis/generators/analysis_generator.cpp"
    "../../native/analysis/generators/bitstream_indexer.cpp"
    "../../native/analysis/parsers/vac2_parser.cpp"
    "../../native/analysis/parsers/vachunk_parser.cpp"
    "../../native/media/private_cdn_flv_demuxer.cpp"
  )
  target_compile_features(VoidPlayerCli PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(VoidPlayerCli PRIVATE /utf-8 /W4 /WX /permissive- /EHsc)
  endif()
  target_compile_definitions(VoidPlayerCli PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
  )
  target_include_directories(VoidPlayerCli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../native"
    "${CMAKE_CURRENT_BINARY_DIR}"
  )
  target_include_directories(VoidPlayerCli SYSTEM PRIVATE
    "${FFMPEG_INCLUDE_DIR}"
  )
  target_link_libraries(VoidPlayerCli PRIVATE
    spdlog::spdlog_header_only
    libzstd_static
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    d3d11
    dxgi
    d3dcompiler
  )
endif(FFMPEG_FOUND)

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)
