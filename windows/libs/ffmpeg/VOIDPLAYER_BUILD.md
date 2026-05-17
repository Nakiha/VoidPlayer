# VoidPlayer FFmpeg Package

This directory contains the VoidPlayer-specific Windows FFmpeg runtime/dev
package built by:

<https://github.com/Nakiha/VoidPlayer-FFmpeg-Build>

Build provenance:

- Repository commit: `960cd22`
- GitHub Actions run: <https://github.com/Nakiha/VoidPlayer-FFmpeg-Build/actions/runs/26040995078>
- Artifact: `voidplayer-ffmpeg-windows-x64-n8.1`
- FFmpeg: `n8.1`
- dav1d: `1.5.3`
- Target: `windows-x64-msvc`

The package intentionally keeps VoidPlayer's runtime library shape:

- `avcodec`, `avformat`, `avutil`, and `swresample`
- file, HTTP/HTTPS, and SFTP input
- FFmpeg's default demuxers, decoders, parsers, and bitstream filters for broad
  playback compatibility
- representative coverage for MP4/MOV, Matroska/WebM, AVI, FLV, MPEG-TS/PS,
  Ogg, ASF, WAV, raw H.264/HEVC/VVC, H.264, HEVC, H.266/VVC, AV1, VP8/VP9,
  MPEG-1/2/4, AAC, AC3/EAC3, DTS/DCA, TrueHD, MP1/MP2/MP3, FLAC, ALAC, APE,
  Opus, Vorbis, and PCM
- D3D11VA/DXVA2 hardware acceleration entries for H.264, HEVC, AV1, and VP9
- no MSYS2/MinGW runtime DLL dependency

Encoders, muxers, command-line programs, avdevice/devices, avfilter/filters,
and swscale are not included.

The macOS artifact from the same run is kept in the FFmpeg build repository's
CI artifacts for now. VoidPlayer is currently Windows-only, so macOS binaries
should not be placed under this Windows package directory.
