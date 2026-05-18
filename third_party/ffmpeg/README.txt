VoidPlayer FFmpeg runtime/dev package

FFmpeg: n8.1
dav1d:  1.5.3
Target: macos-arm64

This package contains avcodec, avformat, avutil, swresample shared
libraries (.dylib), headers, and license material for VoidPlayer.

VideoToolbox hardware acceleration is enabled for H.264, HEVC, VP9,
and AV1 decoding.

Local file and HTTP/HTTPS playback protocols are enabled. Dylib install names
are rewritten to @rpath for app-bundle redistribution.

FFmpeg's broad default demuxer/decoder/parser/bitstream-filter set is enabled
for playback compatibility, including H.266/VVC software decode. Encoders,
muxers, devices, filters, command-line programs, avdevice, and swscale are not
included.

The instrumented analysis FFmpeg submodule is not included.
