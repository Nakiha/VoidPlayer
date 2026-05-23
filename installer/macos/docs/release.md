# macOS Release Notes

VoidPlayer macOS packages are staged by `python dev.py package` on macOS. Pass
`--installer` to also create a local compressed DMG from that staging directory.
The staging directory contains `VoidPlayer.app`, GPL/third-party notices, and
the FFmpeg package README/license metadata copied from `third_party/ffmpeg`.

Current release boundary:

- Minimum macOS version: 14.0, matching the bundled FFmpeg dylib deployment
  target.
- Local-file native playback, VideoToolbox hwdownload, software fallback,
  miniaudio/CoreAudio output, loop/seek/play/pause, and macOS UI smoke coverage
  are supported.
- Network/SSH media and analysis UI/IPC remain disabled by macOS capability
  gates until first-class macOS workflows are implemented.
- `dev.py package` verifies the staged FFmpeg dylib layout with `otool -L`,
  signs the staged app after adding bundled notices, then verifies it with
  `codesign --verify --deep --strict`. By default this uses ad-hoc signing for
  local testing. Pass `--macos-sign-identity`, or set
  `VOIDPLAYER_MACOS_SIGN_IDENTITY`, to use a Developer ID Application identity
  with the hardened runtime and timestamp.
- `dev.py package --installer` creates
  `build/package/macos/installer/VoidPlayer-<version>-macos-arm64.dmg` for local
  testing. Add `--macos-notarize --macos-notary-profile <profile>`, or set
  `VOIDPLAYER_MACOS_NOTARY_PROFILE`, to submit the DMG with `xcrun notarytool`,
  staple the ticket, and validate it with `xcrun stapler`.

Before distributing outside local testing, use the release Developer ID identity,
notarize and staple the DMG, and keep the staged compliance documents with the
distributed package.
