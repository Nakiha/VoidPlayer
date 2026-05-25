# macOS Port Plan

VoidPlayer macOS support must converge on the existing native player pipeline. The macOS runner owns
Cocoa windows, sandbox file access, platform-channel glue, Flutter texture registration, and
`CVPixelBuffer` lifecycle only. Playback policy belongs in shared native code.

Detailed implementation history is intentionally not recorded here; use git history for that. The
current renderer unification work is tracked in
[RENDERER_PLATFORM_BACKEND_PLAN.md](RENDERER_PLATFORM_BACKEND_PLAN.md).

## Current State

- Flutter macOS launches through explicit capability gates.
- Local-file playback uses shared demux/decode queues and the shared `RenderSink`.
- Audio uses the shared native audio engine with the miniaudio/CoreAudio output path.
- FFmpeg dylibs are bundled and staged for macOS builds.
- VideoToolbox initializes through the shared hardware decode provider. H.264/H.265 hardware frames
  can stay in CVPixelBuffer/IOSurface storage for renderer-owned Metal presentation; unsupported
  codecs and formats fall back through the named software path.
- Metal is the normal native presentation target for macOS playback. The remaining transitional
  native tick delegates scheduler-selected present decisions, deadline sleep, and presentation stats
  to the portable `PresentationLoopDriver`; Swift only installs the texture target and forwards
  successful frame notifications to Flutter. Upload failures stay visible in diagnostics instead of
  silently falling back to the old Swift copy path. Renderer-owned upload success/failure counters are
  tracked by native presentation state and surfaced to Dart diagnostics. CVPixelBuffer hardware-frame
  uploads are distinguished from staged YUV package uploads in diagnostics. Step-forward/backward
  commands now call native shared step policy instead of applying fixed PTS deltas in Swift.
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

The active macOS work is no longer "make the MVP smoother". It is:

1. Extract a platform presentation backend seam from the existing Windows renderer without changing
   Windows behavior.
2. Plug macOS Metal/CVPixelBuffer presentation into that shared scheduler.
3. Remove the transitional macOS tick/thread/Swift frame-pump path from normal playback.
4. Move VideoToolbox toward zero-copy CVPixelBuffer/IOSurface presentation.
5. Raise 4K60 expectations only after renderer-owned Metal presentation and zero-copy diagnostics are
   real.

See [RENDERER_PLATFORM_BACKEND_PLAN.md](RENDERER_PLATFORM_BACKEND_PLAN.md) for the phased execution
plan.

## Open Gates

- Renderer-owned macOS presentation must become the normal route.
- Metal shader output must stay aligned with the D3D11 layout/color contract.
- VideoToolbox zero-copy must report explicit fallback reasons.
- Multi-track composition must use the shared present decision and track identity model.
- macOS analysis UI/IPC support still needs a first-class design.
- Windows preservation checks must run on a Windows host after renderer backend boundary changes.

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
