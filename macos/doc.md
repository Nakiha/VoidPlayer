# macOS Runner

This directory contains the Flutter macOS runner and the Swift/Cocoa bridge for
VoidPlayer. It is the platform host for the shared native renderer, not a
separate playback implementation.

The current app minimum is macOS 14.0 because the vendored FFmpeg dylibs in
`../.toolchains/ffmpeg/macos-arm64/lib` are built with `LC_BUILD_VERSION minos 14.0`. The
Xcode runner passes the same deployment target into the native CMake build used
by the app bundle.

`../native/docs/MACOS_READINESS.md` records macOS native readiness gates. This
document describes the current runner contract.

## Current Role

The macOS runner owns:

- Cocoa app lifecycle and Flutter macOS engine/plugin registration.
- Sandbox file picking through `NSOpenPanel`.
- `video_renderer` MethodChannel/EventChannel bridging.
- Exported Flutter surface acquisition for final runner composition.
- `CVPixelBuffer` lifecycle and publication through `MacOSNativeTargetRing`.
- App data/log path setup and macOS crash diagnostics.
- macOS package/sign/notarization staging inputs.
- Security-scoped bookmarks are captured from `NSOpenPanel` selections and
  reactivated before persisted external paths are reopened.

The runner does not own playback scheduling, decode policy, seek/loop behavior,
track lifecycle, layout policy, audio policy, or color conversion.

## Native Boundary

Shared native code owns FFmpeg demux/decode, playback clock, seek/loop,
track lifecycle, layout, `RenderSink`, `PresentDecision`,
`RendererDrawSnapshot`, audio, refresh completion, and failure state.

macOS-specific native code owns the Metal presentation backend:

```text
RendererDrawSnapshot
  -> MetalPresentationBackend::draw_frame()
  -> offscreen BGRA8/RGBA16F CVPixelBuffer / IOSurface target
  -> runner-managed native video layer
  -> runner composition under Flutter's transparent ARGB UI surface
```

Native allocates and publishes one complete viewport target; Swift retains the
latest target and composes it separately from Flutter's UI surface. Viewport
pan/zoom submits the latest layout intent to the shared renderer, which redraws
the native target with layout and overlay already applied. Interaction redraws
are display-link driven and independent of media frame rate; shared native code
arbitrates them against playback presents so the same full target is not drawn
twice. The runner display link samples the retained native target and current
Flutter surface each tick. Swift must not add a playback clock, seek policy,
loop policy, or layout compositor.

The detailed presentation contract is documented in
`../native/docs/MACOS_PRESENTATION_BACKEND.md`; stabilization and local
validation are tracked in `../native/docs/MACOS_READINESS.md`.

## Capabilities

| Capability | Current state |
| --- | --- |
| Native playback | Feature-complete enough for stabilization/release-readiness. |
| Presentation | Runner-composed native Metal video layer behind Flutter ARGB UI. |
| Hardware decode | VideoToolbox preserves decoder-owned CVPixelBuffer/IOSurface frames for the native Metal path; forced hwdownload remains diagnostic fallback only. |
| Software fallback | Supported through explicit YUV/BGRA present packages and fallback diagnostics. |
| Audio | Shared native audio engine and miniaudio output path. |
| Analysis FFI/cache | Native analysis symbols and cache tooling are linked. |
| Analysis UI/IPC | Capability-gated on macOS; do not enable until workflow/design is ready. |
| Network/SSH media | Not part of the current macOS runner contract. |

`PlatformCapabilities` should keep unsupported macOS workflows gated instead of
silently routing them through Windows-only UI or IPC assumptions.

## Validation

Recommended local gates are selected by impact area:

```bash
python dev.py test --native-only
flutter build macos --debug
python dev.py mac-ui-test --build ui_tests/macos/native_facade_smoke.csv
```

Useful macOS smoke areas:

- `native_facade_smoke.csv`: native bridge, metadata, diagnostics.
- `native_vvc_software_playback_smoke.csv`: software decode fallback visibility.
- `native_seek_frame_smoke.csv`: seek refresh through native renderer completion.
- `native_layout_split_smoke.csv`: shared layout through Metal presentation.
- `native_playing_dual_track_pan_smoke.csv`: playing complete-target layout and
  runner composition follow pan updates.
- `native_paused_dual_track_pan_zoom_smoke.csv`: paused complete-target redraw
  follows pan/zoom updates.
- `native_add_track_smoke.csv`: multi-track add/remove/offset diagnostics.
- `native_compositor_lifecycle_stress_smoke.csv`: SDR/HDR target-ring rebuild,
  seek/resize, track compaction, destroy/recreate, and window-close lifecycle.
- `analysis_gated_smoke.csv`: analysis FFI and media-header overlay activation present, external
  analysis UI/IPC still gated.

Native VideoToolbox/provider and macOS presentation smokes are part of the
native test suite where available. If the runner, Swift bridge, macOS backend,
or shared renderer boundary changes, include a Windows preservation pass on a
Windows host before release.

## Logs And Crash Diagnostics

The macOS runner uses the same VoidPlayer log directory selected during Dart
logging initialization. Native terminate/signal handlers write macOS crash logs
beside the Flutter/native logs, while system `.ips` reports still appear under
`~/Library/Logs/DiagnosticReports`.

`dev.py mac-ui-test` treats newly generated `VoidPlayer*.ips` /
`void_player*.ips` reports as failures and prints a short faulting-thread
summary.

## Release Staging

```bash
python dev.py package
python dev.py package --installer
```

On macOS this builds a release app, copies `VoidPlayer.app` into
`build/package/macos/VoidPlayer/`, stages GPL/third-party/FFmpeg compliance
docs, validates bundled FFmpeg `@rpath` linkage with `otool -L`, ad-hoc signs
the staged app with Release entitlements, and verifies the copied app signature.

Before a release candidate, run the macOS package readiness gate:

```bash
python dev.py gate macos-release-readiness
```

This gate stages the app and checks bundled FFmpeg dylibs/symlinks, `@rpath`
linkage, GPL/third-party notices, package cleanliness, sandbox file-picker
inputs, crash-report watcher wiring, codesign verification, and release
entitlements. The default gate accepts ad-hoc signing for local testing; use
`scripts/dev/check_macos_release_readiness.py --require-developer-id` against a
Developer ID signed stage before external distribution. Developer ID
package/readiness runs also require `spctl -a -vv --type execute` to pass for
the staged app.

With `--installer`, the same staging directory is compressed into
`build/package/macos/installer/VoidPlayer-<version>-macos-arm64.dmg` for local
testing. Developer ID signing can be supplied with `--macos-sign-identity` or
`VOIDPLAYER_MACOS_SIGN_IDENTITY`; notarization can be supplied with
`--macos-notarize --macos-notary-profile` or
`VOIDPLAYER_MACOS_NOTARY_PROFILE`.

## Release Gates

- Metal shader/layout/color parity against CPU reference and Windows D3D11.
- Drop/late/present-cadence diagnostics before raising 4K60 release thresholds.
- Windows preservation after shared renderer/backend changes.
- Release staging: FFmpeg dylibs, license notices, crash/log paths, sandbox file
  access, signing, and notarization inputs.
- macOS analysis UI/IPC validation.
