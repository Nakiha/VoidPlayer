set(VIDEO_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../resources/video")

# VTM DecoderApp path for analysis tests (generates VBS3 statistics).
# MSVC output: bin/vs18/msvc-19.50/x86_64/release/DecoderApp.exe
file(GLOB_RECURSE VTM_DECODER_GLOB
    "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/vtm/bin/vs*/DecoderApp.exe")
if(VTM_DECODER_GLOB)
    list(GET VTM_DECODER_GLOB 0 VTM_DECODER_PATH)
else()
    set(VTM_DECODER_PATH "")
endif()

# FFmpeg-based analysis tool path for future VBS3 generation tests.
file(GLOB_RECURSE FFMPEG_ANALYSIS_GLOB
    "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/ffmpeg/bin/windows-x64/void_ffmpeg_analyzer.exe"
    "${CMAKE_CURRENT_SOURCE_DIR}/analysis/vendor/ffmpeg/bin/windows-x64/void_hevc_analyzer.exe")
if(FFMPEG_ANALYSIS_GLOB)
    list(GET FFMPEG_ANALYSIS_GLOB 0 FFMPEG_ANALYZER_PATH)
else()
    set(FFMPEG_ANALYZER_PATH "")
endif()
