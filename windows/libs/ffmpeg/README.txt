VoidPlayer FFmpeg runtime/dev package

FFmpeg: n8.1
dav1d: 1.5.3
Target: windows-x64-msvc

This package is intended for VoidPlayer/windows/libs/ffmpeg.
It contains avcodec, avformat, avutil, swresample, headers, MSVC import
libraries, runtime DLLs, and license material.

D3D11VA/DXVA2 hardware acceleration and HTTP/HTTPS playback are enabled.
SFTP playback is libssh via vcpkg x64-windows-static-md, statically linked.

FFmpeg's broad default demuxer/decoder/parser/bitstream-filter set is enabled
for playback compatibility, including H.266/VVC software decode. Encoders,
muxers, devices, filters, command-line programs, avdevice, and swscale are not
included.

Build configuration and provenance are recorded in VOIDPLAYER_BUILD.md and
voidplayer-ffmpeg-manifest.json.

The instrumented analysis FFmpeg submodule is not included.
