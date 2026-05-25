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

- [x] Keep diagnostics explicit: scheduler kind, presentation backend kind, upload mode, hardware
  decode mode, CPU download state, and software fallback state.
- [x] Add a canary that fails when macOS visible playback is still driven only by the Swift frame
  pump once renderer-owned presentation is expected.
- [x] Add native tests for backend-independent present decision identity, carry-forward, and
  multi-track layout inputs.
- [x] Mark the current `native/macos/native_player_bridge.*` tick thread and Swift callback copy path
  as transitional in code comments and diagnostics.

Progress: shared `PresentationSnapshot` now centralizes present-decision metadata, layout constants,
color defaults, storage kind, and NV12 coded-size scale for backend consumption. The macOS bridge
fills its presentation ABI from this contract, and a native canary covers identity, layout, color,
and odd-dimension NV12 metadata. The macOS transitional thread now reports shared scheduler name,
tick/present/notification counters, cached scheduler-decision availability, present-package upload
count, and package storage. Package copy consumes the scheduler-selected `PresentDecision` when a
tick has published one, and CVPixelBuffer-backed hardware frames are now reported as
`cvpixelbuffer` instead of being collapsed into the staged `yuv` package diagnostic. The Swift
texture pump coalesces duplicate copy requests instead of queuing stale uploads. The macOS native
test matrix also covers present-decision carry-forward identity and offset guards. Swift-side
diagnostics now split visible presentation into renderer-owned presentation and Swift fallback-copy
counters. The macOS facade and 4K VideoToolbox smokes require renderer-owned presentation with zero
Swift fallback copies, while the direct-copy fallback smoke proves the legacy Swift path remains
explicit and isolated. First-frame, seek, step, offset, and layout refreshes now install the same
renderer-owned Metal presentation target and ask native to present the current renderer snapshot;
the Swift direct-copy path is reserved for the explicit Metal-disabled fallback. Metal-enabled
playback now treats an unavailable renderer-owned target as a visible presentation failure instead
of silently entering the direct-copy fallback.

Exit gate: current macOS playback still works, and diagnostics clearly distinguish transitional
texture-pump presentation from renderer-owned presentation.

### P1: Extract A Platform Presentation Backend Interface

Goal: split the Windows renderer loop from D3D11-specific presentation without changing Windows
behavior.

- [x] Define a renderer-owned backend interface that consumes immutable draw snapshots and
  `RenderSink::PresentDecision`.
- [ ] Move D3D11 draw, frame preparation, shared texture publication, capture, and device-loss
  handling behind the Windows implementation of that interface.
- [ ] Keep `Renderer` as the owner of scheduling, playback state, track lifecycle, and render-loop
  timing.
- [ ] Preserve existing Windows FFI, runner, and UI automation behavior.

Progress: the first draw seam is in place. `Renderer` now builds the immutable
`RendererDrawSnapshot` and delegates frame drawing through the `PresentationBackend::draw_frame`
interface. D3D11 implements that contract today; Metal has an explicit unsupported stub until it is
wired to the shared renderer loop. `Renderer` retains ownership of scheduling, playback state, track
lifecycle, GPU wait accounting, and overlay hooks.

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

Progress: macOS track creation no longer hand-builds its own demux/decode/track-buffer stack. The
macOS bridge now uses the shared `TrackPipelineFactory` with `render_backend=Metal`, keeping
VideoToolbox device-mode selection platform-specific while sharing the normal native track pipeline
construction path. Presentation tick bookkeeping, cached present-decision ownership, deadline sleep,
and presentation stats now live in the portable `PresentationLoopDriver`; the separate macOS tick
thread is reduced to a transitional host loop around that shared driver. When the native Metal
presentation target is installed, Swift no longer falls back to the old copy path on upload failure;
failures are counted in diagnostics instead, making renderer-owned presentation problems visible.
Renderer-owned callbacks now publish the native upload result's frame metadata instead of
reconstructing presented PTS/DTS from scheduler stats in Swift. Renderer-owned upload success and
failure counters are owned by the native presentation target and exposed through diagnostics.
macOS step-forward/backward now reuses the portable native step policy and publishes paused-frame
decisions through the shared presentation loop driver. The portable scheduler now keys frame
notifications by the per-slot presented-frame signature instead of only the first selected PTS, and
the macOS bridge applies the shared carry-forward policy before caching a present decision. Multi-track
macOS presentation therefore follows the Windows rule that tracks with no new frame keep their last
valid frame instead of being silently dropped from the current upload.
The host loop itself remains the main cleanup target for this phase.
The transitional macOS host loop now builds a `RendererDrawSnapshot` from the shared scheduler
decision and asks `MetalPresentationBackend::draw_frame` to upload it, instead of calling the older
player-querying copy entrypoint. The host loop still owns thread wakeups for now, but visible
renderer-owned uploads are no longer selected by a separate Swift/player copy path.
The next-frame deadline calculation is now the same offset-aware native policy on Windows and macOS;
the macOS host loop no longer carries its own duplicate offset deadline helper.
Pending multi-track presentation decisions no longer wake the Swift texture bridge or count as
renderer-owned upload failures; only a successfully uploaded renderer-owned frame is reported as a
frame-available callback on the normal Metal path.
The first-frame/paused refresh fallback now uses the shared paused-frame snapshot instead of
peeking a primary track frame through a macOS-private frame-selection snippet.
Renderer-owned upload bookkeeping is centralized in the macOS native player wrapper, so manual
refresh and the transitional host loop share the same pending/success/failure accounting.
Manual renderer-owned refresh now falls back to the shared paused-frame snapshot across all active
tracks instead of peeking only the primary file id.
The frame-available callback publication rule is now a shared presentation-loop policy: scheduler
notifications without a renderer-owned target still publish for explicit fallback paths, but an
installed renderer-owned target must upload successfully before Flutter is notified.

Exit gate: macOS UI smokes pass with `rendererOwnedPresentationActive=true`, and the old tick-driven
presentation path is no longer the normal route.

### P3: Implement Renderer-Owned Metal Presentation

Goal: make Metal do the same job D3D11 does today.

- [ ] Own `MTLDevice`, command queue, `CVMetalTextureCache`, staging resources, and reusable textures
  inside the macOS presentation backend.
- [x] Present BGRA, NV12, planar YUV420, and P010 through a single Metal shader contract derived from
  the D3D11 layout/color constants.
- [x] Preserve track identity, carry-forward, per-track source dimensions, padded stride semantics,
  and multi-track composition.
- [ ] Add deterministic CPU-vs-Metal and D3D11-contract parity tests before raising performance
  thresholds.
- [ ] Expose upload latency, dropped/late frame counts, backend fallback reasons, and actual
  presented-frame cadence in diagnostics.

Progress: macOS exposes a native present-frame package ABI that combines the selected
`PresentDecision`, storage kind, layout metadata, and packed frame bytes for backend consumption.
The Swift-visible backend calls through this ABI, and the native Metal presentation backend can
consume the same package without asking Swift to choose frames. The package path keeps
zero-copy CVPixelBuffer, staged YUV, and BGRA fallback storage distinct in diagnostics.
Software-decoded single-track playback no longer opts out of the renderer-owned Metal target; the
VVC/H.266 macOS smoke locks that path down with zero Swift fallback copies while still reporting
`presentationFallbackReason=software-decode`. The 10-bit H.264/P010 macOS smoke runs through
VideoToolbox renderer-owned CVPixelBuffer presentation instead of forcing the direct-copy hwdownload
path. The native Metal uploader smoke covers synthetic NV12, planar YUV420, and P010 package parity,
including offset/stride shader paths without relying on a large media fixture.
macOS diagnostics now name the upload storage explicitly:
`metal-cvpixelbuffer-present-package`, `metal-yuv-present-package`, or
`metal-bgra-present-package`, so 4K60 zero-copy and software fallback paths no longer look like the
same BGRA upload mode.
The Metal presentation backend now owns a renderer draw target and can consume
`RendererDrawSnapshot` directly, packaging scheduler-selected CPU frames through the same Metal
present-package uploader used by the transitional macOS path. A native smoke covers the shared
snapshot-to-Metal backend draw path with a real CVPixelBuffer target.
`MetalPresentationBackend::draw_frame` also preserves the VideoToolbox CVPixelBuffer fast path when
the shared snapshot contains a single decoder-owned CVPixelBuffer frame, so the bridge handoff does
not regress 4K60 zero-copy presentation.

Exit gate: macOS can present multi-track CPU-decoded frames through renderer-owned Metal without the
Swift pump choosing frames, and shader parity tests cover the supported formats.

### P4: Move VideoToolbox Toward Zero-Copy Presentation

Goal: remove the 4K60 bottleneck caused by hardware decode download-to-CPU.

- [x] Teach the hardware-frame converter to preserve VideoToolbox `CVPixelBuffer` backed frames when
  the macOS presentation backend can consume them.
- [x] Map VideoToolbox NV12/P010 planes into Metal textures through IOSurface/CVMetalTextureCache.
- [x] Keep explicit fallback to download-to-CPU when codec, format, device, or sandbox constraints
  prevent zero-copy.
- [x] Add 4K60 local performance diagnostics that report decode fps, render fps, presented fps,
  upload time, and fallback reason.

Progress: macOS diagnostics now expose primary decode frame count/fps, renderer-owned upload fps,
presented-frame callback fps, upload timing, and `presentationFallbackReason`. The 4K60 VideoToolbox
smoke asserts nonzero decode/upload cadence and `presentationFallbackReason=none` on the
renderer-owned CVPixelBuffer path.

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

Progress: the unused Swift decoded-first-frame object path and its allocating BGRA native bridge
entrypoint have been removed. macOS now initializes visible native media through the presentation
path rather than caching a one-off decoded frame blob in Swift.
The macOS event bridge now retains the `video_renderer/events` channel and emits
`seekPreviewPresented` asynchronously after paused seek publication, removing the Dart timer
fallback from normal macOS seek-settle flow.
Swift no longer stops playback by comparing the presented PTS against the reported duration; EOF
settlement is owned by the native/shared playback policy so bad container durations do not create a
separate macOS timeline rule.
The unused no-layout `VPMacOSMetalUploaderCopyCurrentFrame` blit ABI has also been removed; macOS
Metal uploads now enter through the layout/present-package or decoder-owned CVPixelBuffer paths.
macOS present-package construction has been centralized in `presentation_package_builder`, so the
legacy bridge C ABI and the renderer-owned Metal backend consume the same `RendererDrawSnapshot`
rules for decision metadata, YUV/BGRA package layout, CVPixelBuffer fast-path metadata, stride
guards, and fallback behavior.
Swift presentation counting now records renderer-owned and explicit fallback presentations through
one helper while keeping the existing diagnostics keys stable.
The remaining player-querying Metal uploader ABI has been removed from tests and the C bridge; test
coverage now copies an explicit present package and asks the uploader/backend to render that package.
Unused public bridge entrypoints for raw current-frame BGRA, raw present-frame BGRA/YUV, retained
CVPixelBuffer frame copies, and allocating BGRA frame ownership have been removed; macOS
presentation now exposes either the explicit software fallback canvas copy or the renderer
present-package path.
macOS track add/remove now follows the shared renderer identity rule more closely: tracks receive
monotonic generation ids, removed slots clear their render-sink offsets and cached presentation
decisions, and the native smoke covers remove/re-add so stale offsets cannot leak into a new track.
macOS play/pause now uses the shared renderer playback command policy and shared track decode-state
helper, so resuming after step/seek restores video decode and pause-after-preroll state the same way
as Windows; the playing-step UI smoke now resumes playback and verifies the viewport advances.

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
