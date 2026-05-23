# macOS Release Notes

VoidPlayer macOS packages are staged by `python dev.py package` on macOS. The
staging directory contains `VoidPlayer.app`, GPL/third-party notices, and the
FFmpeg package README/license metadata copied from `third_party/ffmpeg`.

Current release boundary:

- Minimum macOS version: 14.0, matching the bundled FFmpeg dylib deployment
  target.
- Local-file native playback, VideoToolbox hwdownload, software fallback,
  miniaudio/CoreAudio output, loop/seek/play/pause, and macOS UI smoke coverage
  are supported.
- Network/SSH media and analysis UI/IPC remain disabled by macOS capability
  gates until first-class macOS workflows are implemented.
- `dev.py package` ad-hoc signs the staged app after adding bundled notices,
  then verifies it with `codesign --verify --deep --strict`. Developer ID
  signing, DMG creation, and notarization are still manual follow-up steps.

Before distributing outside local testing, sign the staged app with the release
Developer ID identity, notarize the signed artifact, staple the ticket, and keep
the staged compliance documents with the distributed package.
