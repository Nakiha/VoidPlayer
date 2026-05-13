set(VIDEO_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../resources/video")

# FFmpeg-based analysis tool path for overlay compatibility fixture generation.
file(GLOB_RECURSE FFMPEG_ANALYSIS_GLOB
    "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/ffmpeg/bin/windows-x64/void_ffmpeg_analyzer.exe"
    "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/ffmpeg/bin/windows-x64/void_hevc_analyzer.exe")
if(FFMPEG_ANALYSIS_GLOB)
    list(GET FFMPEG_ANALYSIS_GLOB 0 FFMPEG_ANALYZER_PATH)
else()
    set(FFMPEG_ANALYZER_PATH "")
endif()
