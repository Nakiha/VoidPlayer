# macOS Readiness

VoidPlayer macOS support must converge on the existing native player pipeline. The macOS runner owns
Cocoa windows, sandbox file access, platform-channel glue, Flutter texture registration, and
`CVPixelBuffer` lifecycle only. Playback policy belongs in shared native code.

## Current State

- macOS native playback is feature-complete enough to treat the port as a stabilization effort:
  shared scheduling, renderer-owned Metal presentation, VideoToolbox zero-copy, software fallback,
  refresh completion, and per-track diagnostics are all on the normal route.
- Flutter macOS launches through explicit capability gates.
- Local-file playback uses shared demux/decode queues and the shared `RenderSink`.
- Audio uses the shared native audio engine with the miniaudio/CoreAudio output path.
- FFmpeg dylibs are bundled and staged for macOS builds.
- VideoToolbox initializes through the shared hardware decode provider. H.264/H.265 hardware frames
  can stay in CVPixelBuffer/IOSurface storage for renderer-owned Metal presentation; unsupported
  codecs and formats fall back through the named software path.
- Metal is the normal native presentation target for macOS playback. Swift installs the texture
  target, owns `CVPixelBuffer` lifecycle, and forwards successful frame notifications to Flutter;
  playback timing, seek/step/loop, layout, track lifecycle, refresh completion, and failure state live
  in shared native code. Upload failures stay visible in diagnostics instead of silently switching to
  a Swift-side copy path. CVPixelBuffer hardware-frame uploads are distinguished from staged YUV
  and BGRA package uploads in diagnostics.
- The default macOS presentation policy is Auto: SDR media stays on the SDR native-compositor target,
  while PQ/HLG media promotes to the EDR compositor only when the active display reports usable EDR
  headroom.
- macOS analysis FFI can build and answer basic handle/base-generation calls. The macOS dev/CI
  toolchain can also build `void_ffmpeg_analyzer`, generate VAC2 base + overlay VACHUNK through
  portable `VoidPlayerCli`, and reopen the produced cache files. Runtime overlay activation through
  the media header is enabled on macOS; external analysis windows and analysis UI/IPC remain
  capability-gated.

## Hard Contract

- Reuse `NativePlayer`, playback clock, seek behavior, loop behavior, track lifecycle, layout model,
  capture contracts, and diagnostics shape.
- Keep platform differences behind adapters: D3D11/shared texture on Windows,
  Metal/CVPixelBuffer/IOSurface on macOS, and platform audio devices.
- Do not duplicate playback state machines, timeline math, loop enforcement, or track ownership in
  Swift or Objective-C++.
- Do not keep a macOS-only renderer sidecar once the shared renderer backend seam exists.
- Do not introduce `libswscale` or `libyuv` as broad fallback paths.
- Keep software presentation as an explicit fallback and parity oracle, not the silent main route.

## Architecture Target

```text
Dart UI / Actions
  -> NativePlayerController
     -> platform channel glue
        -> shared NativePlayer facade
           -> shared demux / decode / playback / track / layout / render scheduling
              -> platform hardware decode provider
              -> platform presentation backend
```

Windows and macOS should run the same scheduler. The backend decides how a chosen present decision is
converted into a platform texture; it does not decide playback time.

## Stabilization Scope

- Keep renderer-owned Metal presentation and VideoToolbox/software fallback diagnostics truthful.
- Add deterministic Metal shader/layout/color parity coverage before raising performance thresholds.
- Preserve Windows behavior after shared backend boundary changes.
- Keep architecture docs and tests written as current backend contracts.
- Keep release-level package, license, crash/log, file-permission, signing, and notarization
  evidence current without moving playback policy back into Swift.

Use [MACOS_PRESENTATION_BACKEND.md](MACOS_PRESENTATION_BACKEND.md),
[THREADING_MODEL.md](THREADING_MODEL.md), and [TARGET_BOUNDARIES.md](TARGET_BOUNDARIES.md)
for the current backend contracts.

## Validation Gates

- Metal shader output must stay aligned with the D3D11 layout/color contract through deterministic
  parity tests.
- Presentation diagnostics should grow drop/late/present-cadence counters before 4K60 thresholds are
  treated as release gates.
- macOS analysis UI/IPC support must stay capability-gated until validated.
- Windows preservation checks must run on a Windows host after renderer backend boundary changes.
- macOS release-readiness now verifies FFmpeg dylibs, license notices, crash/log watcher wiring,
  sandbox file-picker inputs, package cleanliness, `@rpath`, codesign, and Release entitlements.
  External distribution still needs Developer ID signing and notarization evidence.

## Validation Entry Points

| Change | Minimum validation |
| --- | --- |
| Portable native code | `python dev.py test --native-only` |
| macOS runner or texture path | `flutter build macos --debug` plus targeted `python dev.py mac-ui-test ...` |
| Shared renderer scheduling | macOS native tests plus Windows native/UI preservation checks |
| Metal/color/layout behavior | native parity tests plus targeted macOS UI capture smokes |
| macOS HDR Auto policy | `python dev.py gate macos-ui-smoke` for SDR policy plus `python dev.py gate macos-hdr-edr-smoke` on an EDR-capable display |
| Packaging | `python dev.py gate macos-release-readiness` |

Representative local macOS smoke set:

```bash
python dev.py gate macos-ui-smoke
```

Current stabilization gate:

```bash
python3.12 dev.py gate pr-fast
python3.12 dev.py gate macos-ui-smoke
python3.12 dev.py gate macos-hdr-edr-smoke
```
