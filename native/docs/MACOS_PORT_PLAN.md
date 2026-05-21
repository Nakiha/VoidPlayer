# macOS Port Plan

VoidPlayer macOS support must converge on the existing native player pipeline. The macOS runner is
allowed to own Cocoa windows, MethodChannel glue, sandbox file access, and Flutter texture
registration; it must not grow a second playback backend.

Historical phase notes moved to [archive/MACOS_PORT_PLAN_HISTORY.md](archive/MACOS_PORT_PLAN_HISTORY.md).

## Current Status

- Flutter launches on macOS through a platform bootstrap with explicit capability gates.
- The macOS runner registers `video_renderer` / `video_renderer/events`, creates a
  `FlutterTexture`, and can show synthetic pixels, first decoded FFmpeg frames, seek refreshes, and
  a temporary timer-driven preview playback loop.
- macOS FFmpeg dylibs are bundled and codesigned into the debug app.
- Native macOS CMake builds portable targets: `void_player_portable_core`, `void_media_ffmpeg`,
  `void_macos_preview_decoder`, and smoke tools.
- The preview decoder now lives under `native/macos`, links `void_media_ffmpeg`, and reuses shared
  timestamp and software BGRA conversion helpers.
- The app is still not on the real shared `NativePlayer` path. It has no real frame queue, audio,
  A/V sync, Metal backend, or VideoToolbox decode.

## Hard Contract

- Reuse the `NativePlayer` facade, playback semantics, demux/decode policies, seek behavior, track
  lifecycle, layout model, and capture contracts.
- Keep platform differences behind adapters: D3D11/shared-texture output on Windows,
  CVPixelBuffer/IOSurface/Metal output on macOS, and platform audio devices.
- Treat `native/macos/preview_frame_decoder.*` and the Swift preview timer as scaffolding. New work
  should either delete that scaffolding or move it closer to shared native components.
- Do not duplicate playback state machines, loop/timeline math, or track ownership in Objective-C++
  or Swift runner code.
- Do not introduce `libswscale` / `libyuv` as a generic fallback.

## Architecture Target

```text
Dart UI / Actions
  -> NativePlayerController
     -> platform MethodChannel adapter
        -> thin macOS/Windows runner glue
           -> shared native player facade
              -> shared demux / decode / playback / track / layout pipeline
                 -> platform output backend
                    -> D3D11 shared texture on Windows
                    -> CVPixelBuffer / IOSurface / Metal on macOS
```

## Active Milestones

### M1: Shrink The Preview Bridge

Goal: keep current macOS visible playback working while moving all reusable decode/presentation
logic out of Swift runner code.

- [x] Move FFmpeg first-frame/seek decode into `native/macos`.
- [x] Reuse shared timestamp rescaling.
- [x] Reuse shared software BGRA conversion.
- [ ] Move capture/hash metrics out of Swift into a shared test/diagnostic helper or retire them
  when native capture is available.
- [x] Add native frame queue smokes for synthetic and FFmpeg-decoded software frames.
- [ ] Replace timer-driven target-frame decoding with a native frame queue feeding the texture
  bridge.

### M2: Renderer-Neutral Frame Publication

Goal: split decode output from D3D11 presentation so macOS can consume frames without a second
pipeline.

- [ ] Extract a platform-neutral decoded-frame publication interface from `DecodedFramePublisher`.
- [ ] Split `FrameConverter` into software packing and D3D11 snapshot/presenter pieces.
- [ ] Keep `TrackBuffer`, `RenderSink`, seek policies, and playback clock semantics shared.
- [x] Add CTest coverage that exercises software frame publication -> `TrackBuffer` without D3D11.
- [x] Extend the smoke from synthetic frames to FFmpeg-decoded frames.

### M3: Shared NativePlayer Facade On macOS

Goal: make the macOS MethodChannel call the same native player surface as Windows.

- [ ] Add a macOS native library target instead of direct Xcode `#include` shims.
- [ ] Define a small C/Objective-C++ bridge for create/destroy/open/play/pause/seek/currentPts.
- [ ] Return `available=true` only when the shared facade drives playback state.
- [ ] Keep current preview bridge behind diagnostics until it can be deleted.

### M4: Software Playback MVP

Goal: local-file software playback with correct timing and basic audio.

- [ ] Feed decoded frames into the macOS `CVPixelBuffer` texture bridge from a native queue.
- [ ] Add CoreAudio or miniaudio output behind the existing audio abstraction.
- [ ] Validate pause, seek, step, loop, destroy, and window close.
- [ ] Preserve Windows behavior and tests.

### M5: Metal And Hardware Decode

Goal: improve performance after software playback is correct.

- [ ] Add a Metal/CVPixelBuffer backend with the same responsibilities as D3D11 output.
- [ ] Port shader/color/layout behavior with deterministic pixel tests.
- [ ] Add VideoToolbox behind the hardware decode provider interface.
- [ ] Keep software fallback visible in diagnostics.

### M6: Packaging And Release

Goal: make the app distributable and honest about unsupported features.

- [ ] Resolve the FFmpeg dylib deployment-target mismatch: current dylibs report macOS 14.0 while
  the runner builds for macOS 10.15/11.0.
- [ ] Add release build, signing, notarization, and third-party notice docs.
- [ ] Decide macOS analysis support: native library, helper process, or explicit unsupported gate.
- [ ] Add macOS CI once the native facade path is stable.

## Validation Matrix

| Change | Minimum validation |
| --- | --- |
| Portable native code | `cmake -S native -B native/build-macos-make -G "Unix Makefiles" ...` and `ctest --test-dir native/build-macos-make --output-on-failure` |
| macOS runner/texture | `flutter build macos --debug` and relevant `python dev.py mac-ui-test ...` scripts |
| Dart platform/UI | `flutter analyze` plus targeted Flutter/unit/UI tests |
| Shared native playback behavior | Windows `python dev.py test --native-only` plus macOS portable CTest |
| Packaging | `codesign --verify --deep --strict` and `otool -L` on app artifacts |

Current macOS smoke set:

```bash
python dev.py mac-ui-test \
  ui_tests/macos/synthetic_texture_smoke.csv \
  ui_tests/macos/first_frame_smoke.csv \
  ui_tests/macos/first_frame_controls_smoke.csv \
  ui_tests/macos/seek_frame_smoke.csv \
  ui_tests/macos/preview_playback_smoke.csv
```

## Next Slice

The next implementation slice should be M2: introduce a renderer-neutral frame publication seam and
prove software decode can feed a native frame queue without D3D11. That gives macOS a place to plug
CVPixelBuffer/Metal output into the existing pipeline instead of extending the Swift preview timer.
