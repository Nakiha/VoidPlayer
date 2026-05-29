# macOS Presentation Backend

The macOS normal presentation route is renderer-owned Metal:

```text
RendererDrawSnapshot
  -> MetalPresentationBackend::draw_frame()
  -> renderer-owned BGRA CVPixelBuffer / IOSurface target
  -> Flutter Texture
```

Demux, decode, seek, loop, audio, playback clock, layout, refresh completion,
failure state, and track lifecycle stay in shared native code. Swift owns Cocoa,
sandbox/platform-channel glue, `CVPixelBuffer` lifecycle, Flutter texture
registration, and frame notifications only.

## Normal Route

The shared renderer builds a `RendererDrawSnapshot` from the current
`PresentDecision`. The macOS presentation backend then:

- validates that the renderer, backend, and target are available;
- validates target dimensions and BGRA `CVPixelBuffer` compatibility;
- wraps the target through `CVMetalTextureCache`;
- consumes VideoToolbox `CVPixelBuffer` frames or software/fallback present
  packages;
- runs the Metal layout/color path into the renderer-owned target;
- records upload storage, frame PTS, cadence, and failure diagnostics;
- wakes refresh waiters and asks Swift to mark the Flutter texture available.

The renderer-owned target is a Metal-compatible, IOSurface-backed BGRA
`CVPixelBuffer`. Swift creates/registers it and installs it into native through a
short locked section. Native owns the Metal device, command queue,
`CVMetalTextureCache`, validation, upload, draw, and failure accounting.

## Storage Kinds

| Storage kind | Route | Notes |
| --- | --- | --- |
| VideoToolbox `CVPixelBuffer` | `metal-cvpixelbuffer-present-package` | Zero-copy source preservation for supported H.264/H.265 frames. |
| CPU NV12/P010/planar YUV package | `metal-yuv-present-package` | Software or fallback frames staged for the Metal shader path. |
| BGRA package | `metal-bgra-present-package` | Explicit BGRA fallback/capture/parity path. |
| `cvpixelbuffer-bgra-copy` adapter | fallback/parity oracle | Not the normal playback route. Used for software fallback validation and explicit copy tests. |

Unsupported storage kinds, pixel-buffer mismatches, missing Metal state,
`CVMetalTextureCache` failures, package allocation/copy failures, and
`draw_frame` failures must update native presentation state and `lastDrawError`
instead of silently reporting a healthy renderer.

## Viewport Refresh And Failure Semantics

Swift refresh callers use
`VPMacOSNativePlayerRequestRendererOwnedFrameRefresh(...)` for seek, step,
startup, EOF, and paused redraw refreshes. The call:

- requires a renderer-owned target;
- asks shared native renderer code to redraw the current frame;
- waits on completion keyed by target generation, upload count, failure count,
  and last error;
- returns success, explicit failure, or timeout.

Viewport pan/zoom does not install a Swift-side frame pump. Swift submits the
latest layout intent and keeps the `CVDisplayLink` warm only long enough to
coalesce input. While playback is running, native records the layout revision
and defers drawing to the next normal PTS present, so a playing frame carries
the newest layout without an extra cached redraw. Paused, EOF, startup, seek,
and step refreshes may use display-link ticks to redraw the cached frame, but
the renderer still decides whether the tick is skipped, drawn, failed, or
deferred to playback.

`VPMacOSNativePlayerCopyLastRendererOwnedFrameInfo(...)` and the older
`VPMacOSNativePlayerPresentCurrentFrameToMetalTarget(...)` compatibility path
only expose the most recent successful renderer-owned frame information. They
are not the normal active refresh command.

Frame callbacks and failure callbacks wake waiters after renderer/backend locks
are released. Swift diagnostics must read
`rendererOwnedPresentationState` rather than inferring health from renderer
initialization alone.

## Software Fallback / Parity Adapter

`native/macos/presentation_adapter.*` exposes the
`cvpixelbuffer-bgra-copy` software adapter. It starts at shared
`vr::TextureFrame` storage and copies into caller-provided BGRA rows. This
adapter remains useful as:

- a software fallback when the Metal package route cannot consume a frame;
- a CPU-side parity oracle for channel order, range, matrix, stride, and odd
  dimension behavior;
- a clear unsupported-format failure surface in native tests.

It is no longer the macOS normal playback presentation route. The normal route
is the shared renderer plus `MetalPresentationBackend`.

## Diagnostics Contract

The macOS runner should report health from native state fields, including:

- `rendererInitialized`
- `targetInstalled`
- `backendAvailable`
- `lastDrawSucceeded`
- `consecutiveDrawFailures`
- `lastDrawError`
- `lastSuccessfulFramePtsUs`
- `targetGeneration`
- `targetWidth` / `targetHeight`
- `uploadStorageKind`
- upload/failure counters
- layout intent/present/deferred counters
- cadence counters such as duplicate PTS, large PTS gaps, host interval samples,
  host interval max/p95, drop/error aliases, and renderer-owned presentation
  ratio

Compatibility UI keys may continue to summarize these values, but the source of
truth is native presentation state. A missing target, target clear, backend
unavailable, invalid pixel buffer, unsupported package, or Metal draw failure is
not a healthy renderer-owned presentation state.

## Test Coverage

Portable native tests should keep covering:

- `macos_presentation_adapter_smoke` for the software fallback/parity adapter;
- `macos_metal_uploader_smoke` for target validation and Metal upload behavior;
- shared renderer-owned presentation smoke for target install/clear, refresh
  success, failure, timeout, and recovery;
- `macos_metal_color_layout_parity_smoke` for synthetic
  `RendererDrawSnapshot` -> `MetalPresentationBackend` -> backend capture
  parity across BGRA, NV12, planar YUV420, P010 high-bit packages, odd
  dimensions, padded stride, split layout, and aspect-fit behavior.

macOS UI smoke should assert renderer-owned state, upload storage kind,
fallback reason, last draw error, frame callback/cadence counters, and
per-track diagnostics. It should not treat `rendererOwnedPresentationActive`
alone as proof that a frame was drawn.

## Remaining Gates

- Windows D3D11 capture parity against the same color/layout cases.
- Further drop/late present-cadence diagnostics before raising 4K60 release
  thresholds beyond conservative canary checks.
- Release staging for FFmpeg dylibs, license notices, crash/log paths, sandbox
  file access, signing, and notarization inputs.
- macOS analysis UI/IPC design without adding another playback scheduler.
