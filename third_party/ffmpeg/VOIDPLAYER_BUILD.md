# VoidPlayer FFmpeg Package

This directory contains the VoidPlayer-specific macOS FFmpeg runtime/dev
package built by:

<https://github.com/Nakiha/VoidPlayer-FFmpeg-Build>

Build provenance:

- Repository commit: `960cd22e41c2649526afdd7ebc12466c09f4d3da`
- GitHub Actions run: <https://github.com/Nakiha/VoidPlayer-FFmpeg-Build/actions/runs/26040995078>
- Artifact: `voidplayer-ffmpeg-macos-arm64-n8.1`
- FFmpeg: `n8.1`
- dav1d: `1.5.3`
- Target: `macos-arm64`

The package intentionally keeps VoidPlayer's runtime library shape:

- `avcodec`, `avformat`, `avutil`, and `swresample`
- cache, concat, file, HTTP/HTTPS, and pipe input
- FFmpeg's default demuxers, decoders, parsers, and bitstream filters for broad
  playback compatibility
- representative coverage for MP4/MOV, Matroska/WebM, AVI, FLV, MPEG-TS/PS,
  Ogg, ASF, WAV, raw H.264/HEVC/VVC, H.264, HEVC, H.266/VVC, AV1, VP8/VP9,
  MPEG-1/2/4, AAC, AC3/EAC3, DTS/DCA, TrueHD, MP1/MP2/MP3, FLAC, ALAC, APE,
  Opus, Vorbis, and PCM
- VideoToolbox hardware acceleration
- `@rpath` install names for dylib runtime loading

Encoders, muxers, command-line programs, avdevice/devices, avfilter/filters,
and swscale are not included.

This is the default non-Windows FFmpeg root used by
`native/cmake/FFmpeg.cmake`. VoidPlayer is still primarily Windows-oriented;
keeping this under `third_party/ffmpeg` matches the existing CMake fallback
without inventing a second macOS-specific dependency path.
