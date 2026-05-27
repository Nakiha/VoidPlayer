set(VIDEO_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../resources/video")

# FFmpeg-based analysis tool path for overlay compatibility fixture generation.
if(WIN32)
    set(_VOID_FFMPEG_ANALYZER_PLATFORM_DIR "windows-x64")
    set(_VOID_FFMPEG_ANALYZER_EXE "void_ffmpeg_analyzer.exe")
    set(_VOID_FFMPEG_LEGACY_ANALYZER_EXE "void_hevc_analyzer.exe")
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_VOID_FFMPEG_ANALYZER_PLATFORM_DIR "macos-arm64")
    set(_VOID_FFMPEG_ANALYZER_EXE "void_ffmpeg_analyzer")
    set(_VOID_FFMPEG_LEGACY_ANALYZER_EXE "void_hevc_analyzer")
elseif(APPLE)
    set(_VOID_FFMPEG_ANALYZER_PLATFORM_DIR "macos-x64")
    set(_VOID_FFMPEG_ANALYZER_EXE "void_ffmpeg_analyzer")
    set(_VOID_FFMPEG_LEGACY_ANALYZER_EXE "void_hevc_analyzer")
else()
    set(_VOID_FFMPEG_ANALYZER_PLATFORM_DIR "")
    set(_VOID_FFMPEG_ANALYZER_EXE "void_ffmpeg_analyzer")
    set(_VOID_FFMPEG_LEGACY_ANALYZER_EXE "void_hevc_analyzer")
endif()

file(GLOB_RECURSE FFMPEG_ANALYSIS_GLOB
    "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/ffmpeg/bin/${_VOID_FFMPEG_ANALYZER_PLATFORM_DIR}/${_VOID_FFMPEG_ANALYZER_EXE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/ffmpeg/bin/${_VOID_FFMPEG_ANALYZER_PLATFORM_DIR}/${_VOID_FFMPEG_LEGACY_ANALYZER_EXE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/ffmpeg/bin/windows-x64/void_ffmpeg_analyzer.exe"
    "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/ffmpeg/bin/windows-x64/void_hevc_analyzer.exe")
if(FFMPEG_ANALYSIS_GLOB)
    list(GET FFMPEG_ANALYSIS_GLOB 0 FFMPEG_ANALYZER_PATH)
else()
    set(FFMPEG_ANALYZER_PATH "")
endif()
