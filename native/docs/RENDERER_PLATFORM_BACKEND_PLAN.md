# Renderer Platform Backend Unification Plan

VoidPlayer should have one native playback scheduler and multiple platform output backends.
Windows and macOS may differ in decoder devices, GPU resources, texture export, and OS window glue,
but they must not differ in track lifecycle, seek, loop, clock, multi-track present decisions, layout,
or playback state machines.

This plan supersedes the early macOS MVP texture-pump direction. The MVP path remains available only
as a temporary compatibility fallback while the renderer-owned macOS backend is brought up.

## Target Shape

```text
Dart UI / Actions
  -> NativePlayerController
     -> platform channel glue
        -> shared NativePlayer facade
           -> shared playback controller
           -> shared Renderer scheduler
              -> shared RenderSink / PresentDecision / layout constants
              -> platform decode hardware provider
                 -> D3D11VA on Windows
                 -> VideoToolbox on macOS
              -> platform presentation backend
                 -> D3D11 shared texture on Windows
                 -> Metal / CVPixelBuffer / IOSurface on macOS
```

The shared renderer loop owns presentation cadence. Platform backends prepare resources and present
the already selected `RenderSink::PresentDecision`; they do not choose playback time or own track
state.

## Non-Negotiable Boundaries

- One playback clock, loop policy, seek policy, and track lifecycle path.
- One render scheduling path for Windows and macOS.
- `RenderSink::PresentDecision`, track identity, carry-forward behavior, and layout constants remain
  platform-neutral.
- Swift and Objective-C++ runner code may own Cocoa windows, sandbox access, `FlutterTexture`
  registration, `CVPixelBuffer` lifecycle, and frame notifications only.
- macOS code must not add decode, seek, loop, audio, or clock policy outside shared native code.
- D3D11 and Metal shader behavior must be compared from the same layout/color contract.
- Software presentation remains an explicit fallback, not the hidden main path.
- VideoToolbox zero-copy starts only after the renderer-owned Metal backend can present CPU-decoded
  frames deterministically.

## Migration Phases

### P0: Lock The Contract

Goal: make the current split honest before moving code.

- [ ] Keep diagnostics explicit: scheduler kind, presentation backend kind, upload mode, hardware
  decode mode, CPU download state, and software fallback state.
- [ ] Add a canary that fails when macOS visible playback is still driven only by the Swift frame
  pump once renderer-owned presentation is expected.
- [x] Add native tests for backend-independent present decision identity, carry-forward, and
  multi-track layout inputs.
- [x] Mark the current `native/macos/native_player_bridge.*` tick thread and Swift callback copy path
  as transitional in code comments and diagnostics.

Progress: shared `PresentationSnapshot` now centralizes present-decision metadata, layout constants,
color defaults, storage kind, and NV12 coded-size scale for backend consumption. The macOS bridge
fills its presentation ABI from this contract, and a native canary covers identity, layout, color,
and odd-dimension NV12 metadata. The macOS transitional thread now reports shared scheduler name and
tick/present/notification counters. The macOS native test matrix also covers present-decision
carry-forward identity and offset guards.

Exit gate: current macOS playback still works, and diagnostics clearly distinguish transitional
texture-pump presentation from renderer-owned presentation.

### P1: Extract A Platform Presentation Backend Interface

Goal: split the Windows renderer loop from D3D11-specific presentation without changing Windows
behavior.

- [ ] Define a renderer-owned backend interface that consumes immutable draw snapshots and
  `RenderSink::PresentDecision`.
- [ ] Move D3D11 draw, frame preparation, shared texture publication, capture, and device-loss
  handling behind the Windows implementation of that interface.
- [ ] Keep `Renderer` as the owner of scheduling, playback state, track lifecycle, and render-loop
  timing.
- [ ] Preserve existing Windows FFI, runner, and UI automation behavior.

Exit gate: Windows native tests and representative UI smokes pass with no observable behavior change.

### P2: Plug macOS Into The Shared Renderer Scheduler

Goal: delete the separate macOS playback tick as a presentation scheduler.

- [ ] Add a macOS renderer configuration path that creates the shared `Renderer` with a Metal-capable
  presentation backend.
- [ ] Route macOS create/open/play/pause/seek/step/loop/layout/capture calls through the same
  `NativePlayer` and `Renderer` command surface as Windows.
- [ ] Replace `run_tick_thread()` / `tick_playback()` as the source of visible frame callbacks with
  renderer-loop publication.
- [ ] Keep Swift limited to texture registration, `CVPixelBuffer` ownership, and forwarding frame
  availability to Flutter.
- [ ] Keep the current software adapter callable as the renderer fallback while Metal parity is
  incomplete.

Exit gate: macOS UI smokes pass with `rendererOwnedPresentationActive=true`, and the old tick-driven
presentation path is no longer the normal route.

### P3: Implement Renderer-Owned Metal Presentation

Goal: make Metal do the same job D3D11 does today.

- [ ] Own `MTLDevice`, command queue, `CVMetalTextureCache`, staging resources, and reusable textures
  inside the macOS presentation backend.
- [ ] Present BGRA, NV12, planar YUV420, and P010 through a single Metal shader contract derived from
  the D3D11 layout/color constants.
- [ ] Preserve track identity, carry-forward, per-track source dimensions, padded stride semantics,
  and multi-track composition.
- [ ] Add deterministic CPU-vs-Metal and D3D11-contract parity tests before raising performance
  thresholds.
- [ ] Expose upload latency, dropped/late frame counts, backend fallback reasons, and actual
  presented-frame cadence in diagnostics.

Exit gate: macOS can present multi-track CPU-decoded frames through renderer-owned Metal without the
Swift pump choosing frames, and shader parity tests cover the supported formats.

### P4: Move VideoToolbox Toward Zero-Copy Presentation

Goal: remove the 4K60 bottleneck caused by hardware decode download-to-CPU.

- [ ] Teach the hardware-frame converter to preserve VideoToolbox `CVPixelBuffer` backed frames when
  the macOS presentation backend can consume them.
- [ ] Map VideoToolbox NV12/P010 planes into Metal textures through IOSurface/CVMetalTextureCache.
- [ ] Keep explicit fallback to download-to-CPU when codec, format, device, or sandbox constraints
  prevent zero-copy.
- [ ] Add 4K60 local performance diagnostics that report decode fps, render fps, presented fps,
  upload time, and fallback reason.

Exit gate: local 4K60 HEVC/H.264 samples report zero-copy presentation when supported, and fallback
cases are visible rather than silent.

### P5: Remove MVP Leftovers

Goal: leave one maintainable native architecture.

- [ ] Delete obsolete macOS-only preview decoders, frame-copy schedulers, and duplicate diagnostics.
- [ ] Fold remaining macOS presentation adapter tests into renderer backend tests, keeping the
  software adapter as a named fallback oracle.
- [ ] Update architecture, threading, decode, color, and build docs to describe the platform backend
  model.
- [ ] Re-run the macOS smoke set and schedule Windows preservation checks on a Windows host.

Exit gate: documentation and diagnostics describe one shared renderer with platform backends, not a
Windows renderer plus macOS sidecar path.

## Validation Strategy

- Native portable: `python dev.py test --native-only`
- macOS runner: `flutter build macos --debug`
- macOS UI: targeted `python dev.py mac-ui-test ...` scripts, including 4K canary and layout/seek
  smokes
- Windows preservation: Windows `python dev.py test --native-only`, `flutter build windows --release`,
  and at least one smoke UI script after P1/P2-level changes
- Performance: local 4K60 samples are manual/perf gates until automated headed macOS performance
  testing is reliable

## Immediate Next Slice

Start with P0 and P1. The first code change should not add new Metal capabilities. It should extract
the platform presentation backend seam from the existing Windows renderer while preserving D3D11
behavior exactly. Once that seam is real, macOS can plug into the same scheduler instead of extending
the transitional Swift frame pump.
