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
- when analysis overlay is active, high-refresh presentation samples retained
  per-track overlay layers during final composite; CU line layers retain
  direction markers and decode them to fixed screen-pixel black-edge/bright-center
  strokes at composite time, so zoom/pan does not re-raster target-size masks;
  the immediate primitive overlay path is a compatibility/fallback path and must
  not keep growing as the high-refresh solution;
- records upload storage, frame PTS, cadence, and failure diagnostics;
- wakes refresh waiters and asks Swift to mark the Flutter texture available.

The renderer-owned target is a Metal-compatible, IOSurface-backed BGRA
`CVPixelBuffer`. Swift creates/registers it and installs it into native through a
short locked section. Native owns the Metal device, command queue,
`CVMetalTextureCache`, validation, upload, draw, and failure accounting.

## Native Compositor Auto Policy

The HDR path uses a native `CAMetalLayer` compositor above Flutter. Flutter still
owns controls, hit testing, and the transparent viewport hole; the native layer
draws video underneath and composites the exported Flutter surface back on top.

On macOS the default request is Auto:

- SDR-only media uses `native-compositor-sdr`: BGRA renderer-owned target,
  `MTLPixelFormatBGRA8Unorm`, no EDR layer.
- PQ/HLG media uses `native-compositor-edr` when the display reports potential
  EDR headroom: `kCVPixelFormatType_64RGBAHalf`, `MTLPixelFormatRGBA16Float`,
  `wantsExtendedDynamicRangeContent`, and extended linear Display P3.
- If HDR media is opened on a display without EDR headroom, Auto remains on the
  SDR compositor and reports `macOSPresentationReason=auto-hdr-display-unavailable`.

`VOIDPLAYER_MACOS_PRESENTATION_MODE` can force `flutter-texture-sdr`,
`native-compositor-sdr`, or `native-compositor-edr` for diagnostics and
bisecting. Product defaults should rely on Auto.

`VOIDPLAYER_MACOS_PRESENTATION_MODE=wgpu-metal` selects the experimental
renderer-owned wgpu/Metal presentation backend. This is a macOS canary only:
the shared renderer, playback scheduler, target ring lifecycle, and software
package inputs remain unchanged, while the render core is routed through the
Rust/wgpu FFI when it is linked. The backend owns one Rust `WgpuMetalRenderer`
for its lifecycle, so the wgpu device/queue/sampler/bind layout/pipeline are
created during backend initialization and reused across draws. The source BGRA
texture array plus params/package/overlay storage buffers are cached on the same
renderer and grow only when the target size or buffer capacity requires it. The
canary imports the renderer-owned destination `MTLTexture` through `wgpu-hal`
Metal interop. BGRA package layout/split/background sampling runs through a WGSL
render pass over a per-track texture-array source, with the imported
destination used as the render attachment. CPU NV12/P010/planar YUV420P packages
upload their raw package bytes as a WGSL storage buffer; the shader samples
luma/chroma planes, handles P010 unpacking, and applies the basic SDR
range/matrix conversion before writing the same imported destination.
The backend creates the Rust/wgpu renderer first, borrows wgpu's internal
Metal `MTLDevice`, and uses that same device for `CVMetalTextureCache`.
Destination imports assert that the renderer-owned `MTLTexture` was created by
that device before passing it to `wgpu-hal`; a mismatch is reported as an
explicit draw failure. CVPixelBuffer frame sets can also import each source Y/UV
plane as an `MTLTexture` via the same cache and `wgpu-hal`, avoiding CPU readback
in the backend when every present frame is a supported NV12/P010 CVPixelBuffer.
The Rust renderer requests `TEXTURE_FORMAT_16BIT_NORM` during device creation
so P010 source planes can be imported as `R16Unorm`/`RG16Unorm`; adapters that
cannot provide it fail closed during wgpu-metal initialization.
The C++/Rust request ABI carries an explicit wgpu output target descriptor:
`Bgra8Sdr` maps the renderer-owned `kCVPixelFormatType_32BGRA` target to
`MTLPixelFormatBGRA8Unorm`, while `Rgba16FloatEdr` maps the
`kCVPixelFormatType_64RGBAHalf`/`MTLPixelFormatRGBA16Float` EDR target shape.
The Rust renderer owns separate BGRA8 and RGBA16Float final composite pipelines.
For SDR targets the WGSL color path tone-maps PQ/HLG/BT.2020 input into SDR;
for EDR targets it maps SDR/PQ/HLG sources into extended-linear Display-P3
before writing the imported RGBA16Float destination. This is the first HDR/EDR
canary slice; stronger EDR capture/parity evidence and headed HDR display gates
remain follow-up work before wgpu-metal can replace Metal by default. The local
wgpu gate entry points are `python dev.py gate macos-wgpu-metal-smoke` and, on
an EDR-capable display, `python dev.py gate macos-wgpu-metal-edr-smoke`.
The macOS player wgpu-metal canary now honors the normal decode preference:
`preferHardware` uses VideoToolbox source import when available, while
`forceSoftware` and `VOIDPLAYER_DISABLE_VIDEOTOOLBOX=1` keep the software/package
fallback path available for parity smoke tests. Analysis overlay
fill/outline/motion primitives are passed as plain rect buffers and baked into a
Rust-retained per-track overlay texture-array layer keyed by the overlay package
`cache_generation`; the final composite pass samples that retained layer instead
of directly scanning primitives. The wgpu canary does not use the CPU BGRA
overlay fallback. The Rust renderer now retains the last successful source cache
inside the `WgpuMetalRenderer`: package draws keep the uploaded BGRA/source
package buffers, and CVPixelBuffer draws keep the imported per-track Y/UV source
textures. When a later `viewport_composite` draw has the same source signature,
C++ sends only the updated projection/layout decision and overlay generation
through the retained-composite FFI entry; Rust rewrites params as needed and
composites from the retained source/overlay caches without rebuilding the video
source.
The backend follows the same renderer-owned async completion contract
and target-ring ownership shape as Metal: draws acquire an available target,
avoid displayed/protected buffers, mark successful draws completed after wgpu
queue work has finished, and report ring backpressure explicitly instead of
silently reusing a busy target. The backend fails closed with diagnostics if
the Rust FFI, package storage, source import, or destination import path is
unavailable. It also validates the renderer-owned target dimensions and BGRA
pixel format before importing the destination `MTLTexture`, so target contract
violations fail with a stable diagnostic instead of relying on lower-level
interop errors.

## Storage Kinds

| Storage kind | Route | Notes |
| --- | --- | --- |
| VideoToolbox `CVPixelBuffer` | `metal-cvpixelbuffer-present-package` | Zero-copy source preservation for supported H.264/H.265 frames. |
| CPU NV12/P010/planar YUV package | `metal-yuv-present-package` | Software or fallback frames staged for the Metal shader path. |
| BGRA package | `metal-bgra-present-package` | Explicit BGRA fallback/capture/parity path. |
| wgpu-metal package | `wgpu-metal` canary | Rust imports the destination `MTLTexture`; BGRA layout/split, NV12/P010/YUV420P plane sampling/basic SDR conversion, and analysis overlay primitive compositing run in WGSL. |
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
coalesce input. Video source updates stay media-clock driven: the renderer still
selects frames by PTS and the backend records the last completed source package
or VideoToolbox `CVPixelBuffer` decision. Viewport/layout composite may run on
display-link ticks against that retained source, so pan, zoom, split, and
overlay movement are not forced to wait for the next decoded video frame.
Overlay scene generation and GPU layer rasterization are event-driven by
frame/chunk/mode changes; display-link ticks should reuse completed overlay
layers and publish only the final composite.
Paused, EOF, startup, seek, and step refreshes use the same renderer-owned
completion path; the renderer still decides whether a tick is skipped, drawn,
failed, or merged into a newer layout intent.

`VPMacOSNativePlayerCopyLastRendererOwnedFrameInfo(...)` and the older
`VPMacOSNativePlayerPresentCurrentFrameToMetalTarget(...)` compatibility path
only expose the most recent successful renderer-owned frame information. They
are not the normal active refresh command.

## Viewport Interaction Pipeline (Pan / Zoom)

Interactive pan/zoom in native-compositor mode never waits on a renderer
round-trip per input event. Dart updates the full current layout immediately,
then pushes the complete source projection to the compositor. The compositor
therefore has a single projection path: source-resolution RGB buffers plus the
current layout projection. There is no residual transform, revision-anchored
clear, or periodic flush loop.

- **Source projection**: Dart computes per-slot projection values from the
  current `LayoutState` and sends them through
  `prepareNativeCompositorSourceCache`. Repeated calls with the same track
  signature update only projection; they do not reallocate source buffers.
- **Paused sub-mode**: the source ring bakes each track's current frame once
  into renderer-converted RGB/EDR buffers. Since frames do not advance, that
  snapshot remains valid for the whole interaction.
- **Playing sub-mode**: the source ring stays subscribed during interaction
  and re-bakes from frame callbacks, so newly revealed pixels are available
  without renderer layout flushes.
- **Commit**: pointer-up applies the authoritative layout to the renderer once,
  then clears the source ring. The fallback renderer-owned viewport target now
  carries the same full layout the compositor was already projecting.
- **Playback transitions**: play/pause/step call
  `MainWindowLayoutCoordinator.onPlaybackStateChanged`, which flushes any
  deferred interaction layout before playback content changes.

Structural layout changes (mode toggle, zoom set, split drag, resize, focus
jumps) clear the source ring and flush immediately; they intentionally bypass
the interaction path because their content change is discontinuous anyway.

Frame callbacks and failure callbacks wake waiters after renderer/backend locks
are released. Swift diagnostics must read
`rendererOwnedPresentationState` rather than inferring health from renderer
initialization alone.

## Software Fallback / Parity Adapter

`native/macos/presentation/presentation_adapter.*` exposes the
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
- last package copy/gpu-wait/total timing counters
- adapter/backend/device diagnostics and required feature gates such as
  wgpu-metal 16-bit normalized texture support; wgpu-metal also mirrors the
  linked Rust FFI ABI version into the existing `feature_level` diagnostic so
  runner logs can identify C++/Rust ABI skew. ABI v5 carries explicit output
  format/color-mode fields, retained-source composite requests, and overlay
  generation keys across the C++/Rust render boundary; ABI v9 exposes the
  wgpu-owned Metal device for `CVMetalTextureCache` plus destination texture
  identity checks.
- layout intent/present/deferred counters
- source update, viewport composite, source-cache hit/miss, and ring-pressure
  counters
- async publish active state plus command completion p95/failure counters;
  wgpu-metal maps its waited wgpu queue work to the same diagnostic fields
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
- `macos_metal_presentation_backend_smoke` also exercises the wgpu-metal
  factory, explicit fail-closed behavior, BGRA channel order, layout/split,
  retained overlay layer WGSL composite, NV12/P010/YUV420P package conversion,
  P010 `TEXTURE_FORMAT_16BIT_NORM` feature gating, RGBA16Float EDR target
  import, P010 PQ/BT.2020 SDR tone-map and EDR output smoke, async completion,
  target-ring displayed/protected/release state transitions, full/region
  capture, and source CVPixelBuffer NV12/P010 import through
  `CVMetalTextureCache`, `wgpu-hal`, WGSL sampling, and imported destination
  `MTLTexture`. Its wgpu coverage includes CPU limited/full range color
  reference checks for NV12 and P010 plus a Metal parity path for
  source-CVPixelBuffer import.

macOS UI smoke should assert renderer-owned state, upload storage kind,
fallback reason, last draw error, frame callback/cadence counters, and
per-track diagnostics. It should not treat `rendererOwnedPresentationActive`
alone as proof that a frame was drawn.

## Release Gates

- Windows D3D11 capture parity against the same color/layout cases.
- Further drop/late present-cadence diagnostics before raising 4K60 release
  thresholds beyond conservative canary checks.
- Release staging for FFmpeg dylibs, license notices, crash/log paths, sandbox
  file access, signing, and notarization inputs.
- macOS analysis UI/IPC must not add another playback scheduler.
