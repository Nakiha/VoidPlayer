# macOS Port Plan

VoidPlayer macOS support must converge on the existing native player pipeline. The macOS runner owns
Cocoa windows, sandbox file access, platform-channel glue, Flutter texture registration, and
`CVPixelBuffer` lifecycle only. Playback policy belongs in shared native code.

Detailed implementation history is intentionally not recorded here; use git history for that. The
current renderer unification work is tracked in
[RENDERER_PLATFORM_BACKEND_PLAN.md](RENDERER_PLATFORM_BACKEND_PLAN.md).

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
  in shared native code. Upload failures stay visible in diagnostics instead of silently falling back
  to the old Swift copy path. CVPixelBuffer hardware-frame uploads are distinguished from staged YUV
  and BGRA package uploads in diagnostics.
- macOS analysis FFI can build and answer basic handle/base-generation calls, while analysis windows
  and overlays remain capability-gated.

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

## Active Work

The active macOS work is no longer "make the MVP smoother" or "wire macOS into shared renderer".
That path is now feature-complete. The active work is stabilization:

1. Keep renderer-owned Metal presentation and VideoToolbox/software fallback diagnostics truthful.
2. Add deterministic Metal shader/layout/color parity coverage before raising performance thresholds.
3. Preserve Windows behavior after shared backend boundary changes.
4. Convert remaining architecture docs and tests from migration notes to current backend contracts.
5. Prepare release-level package, license, crash/log, and file-permission checks without moving
   playback policy back into Swift.

See [RENDERER_PLATFORM_BACKEND_PLAN.md](RENDERER_PLATFORM_BACKEND_PLAN.md) for the phased execution
plan.

## Open Gates

- Metal shader output must stay aligned with the D3D11 layout/color contract through deterministic
  parity tests.
- Presentation diagnostics should grow drop/late/present-cadence counters before 4K60 thresholds are
  treated as release gates.
- macOS analysis UI/IPC support still needs a first-class design.
- Windows preservation checks must run on a Windows host after renderer backend boundary changes.
- Release staging must verify FFmpeg dylibs, license notices, crash/log paths, sandbox file access,
  and package signing/notarization inputs.

## Validation Entry Points

| Change | Minimum validation |
| --- | --- |
| Portable native code | `python dev.py test --native-only` |
| macOS runner or texture path | `flutter build macos --debug` plus targeted `python dev.py mac-ui-test ...` |
| Shared renderer scheduling | macOS native tests plus Windows native/UI preservation checks |
| Metal/color/layout behavior | native parity tests plus targeted macOS UI capture smokes |
| Packaging | `python dev.py package` |

Representative local macOS smoke set:

```bash
python dev.py mac-ui-test \
  ui_tests/macos/native_facade_smoke.csv \
  ui_tests/macos/native_playback_smoke.csv \
  ui_tests/macos/native_seek_frame_smoke.csv \
  ui_tests/macos/native_loop_range_smoke.csv \
  ui_tests/macos/native_audio_play_seek_smoke.csv \
  ui_tests/macos/native_layout_split_smoke.csv \
  ui_tests/macos/native_4k60_playback_smoke.csv
```

Current stabilization gate:

```bash
python3.12 dev.py test --native-only
python3.12 dev.py build --flutter
python3.12 dev.py mac-ui-test --build \
  ui_tests/macos/native_facade_smoke.csv \
  ui_tests/macos/native_4k60_playback_smoke.csv \
  ui_tests/macos/native_vvc_software_playback_smoke.csv \
  ui_tests/macos/native_add_track_smoke.csv
```
