# Windows Presentation Backend

This document defines the Windows presentation contract. Shared renderer
ownership and color rules remain in
[ARCHITECTURE.md](ARCHITECTURE.md) and [COLOR_PIPELINE.md](COLOR_PIPELINE.md).

The current Windows route is runner-owned presentation with wgpu-owned
composition. SDR is the product route; forced scRGB is retained as a diagnostic
route until Flutter's SDR UI can remain system-managed while HDR video/source
content is presented separately:

```text
Flutter engine fork
  -> complete premultiplied Flutter UI surface export
Windows runner / platform backend
  -> D3D12 shared texture/fence/generation acquisition
  -> HWND, DirectComposition/DXGI target, HDR/SDR policy, device recovery
WgpuD3D12PresentationBackend / Rust WgpuRenderCore
  -> imports Flutter UI + video/source textures on SDR or diagnostic scRGB paths
  -> performs color/layout/split/pan/zoom/overlay/Flutter composition
  -> outputs the final target
Windows runner / platform backend
  -> presents to the window
```

The old Flutter D3D11 SRV composition pass, readback-only diagnostics, retained
D3D11 source/Flutter graph, runner source-cache bridge, and runner overlay
bridge have been removed from the product route. They must not be reintroduced
as workarounds for Flutter pacing or projection issues.

## Current Product And Experimental Routes

This section describes the active wgpu/D3D12 migration state. New composition
work must move toward `WgpuD3D12PresentationBackend`/Rust wgpu instead of
adding shader or retained graph behavior to `WindowsNativeCompositor`.

The default Windows product policy is now `auto` and requires the locked
VoidPlayer Flutter engine:

```text
shared RendererDrawSnapshot
  -> WgpuD3D12PresentationBackend / Rust WgpuRenderCore
  -> D3D12 video/source imports + D3D12 Flutter UI surface import
  -> final BGRA8 SDR output resource
  -> WindowsNativeCompositor present bridge
  -> DComp/DXGI presentation to the Flutter HWND
```

SDR-only sessions use `native-compositor-sdr`. A session containing an active
PQ/HLG track on an HDR output keeps `windowsPresentationDesiredMode` at
`native-compositor-scrgb`, but currently stays on `native-compositor-sdr` with
`windowsPresentationFallbackReason=hdr-ui-composition-unsupported`. This avoids
shipping the known-wrong path where the full Flutter SDR UI is shader-composited
into an HDR/scRGB target instead of receiving the system-managed SDR role. The
runner obtains Flutter's DXGI adapter and the resolved output adapter and passes
both into the native compositor. Hosted CI may
explicitly enable
`VOIDPLAYER_ALLOW_D3D11_HEADLESS_WARP_FALLBACK=1`; that is launch/contract
coverage, not release evidence for a desktop GPU.

The compatibility request `sdr` is now an alias for `native-compositor-sdr`.
`native-compositor-sdr` and `native-compositor-scrgb` remain explicit
diagnostic modes; forced scRGB does not depend on the current Windows HDR
toggle. `flutter-texture-sdr`, `flutter`, `fp16-scrgb`, and unknown Windows
presentation requests fail closed instead of creating a Flutter Texture video
route.

The forced HDR compositor diagnostic mode remains:

```powershell
$env:VOIDPLAYER_WINDOWS_PRESENTATION_MODE="native-compositor-scrgb"
python dev.py launch
```

The Auto and forced native-compositor routes require the locked VoidPlayer
Flutter local engine. The engine exports Flutter's final premultiplied-alpha
frame through the D3D12 surface export ABI v2: shared texture handle, fence,
and generation. The wgpu/D3D12 render core imports that surface together with
video/source textures, performs color/layout/split/pan/zoom/overlay and
Flutter premultiplied-alpha composition, then writes a final target. The Windows
runner owns HWND/DComp/DXGI, display/HDR/SDR policy, device-loss recovery, and
presenting the final target to the window. It does not use a key color,
`WS_EX_LAYERED`, window capture, a child HWND sandwich, or a rectangular native
hole.

Runtime track changes and debounced display/window notifications rerun the same
resolver. Target changes create a candidate output target, render one valid
wgpu/D3D12 frame into it, then atomically replace the DComp visual content.
HDR candidate failure falls back to native SDR; failure of the SDR compositor
enters an explicit failed state and does not restore Flutter Texture SDR.

If the active output belongs to a different adapter, the renderer and Flutter
export remain on the producer adapter while `WindowsNativeCompositor` recreates
its D3D12/DComp target on the output adapter. Cross-adapter support must remain
an explicit D3D12 GPU-copy or shared-handle transport with diagnostics; it must
not fall back to CPU readback, screenshots, or private color transforms. The
default synchronization path waits on producer fence evidence.
`VOIDPLAYER_WINDOWS_CROSS_ADAPTER_SYNC=shared-fence` requests the optional
shared-fence path for local evidence; initialization or capability failure
falls back to the diagnosed default path.

Once Dart publishes a valid projection signature, the render path maintains an
atomic source-resolution bundle with up to four FP16 scRGB textures. The
projection state is consumed by the wgpu/D3D12 render core so pan, zoom,
side-by-side order, split position, background, overlay, and Flutter UI are
composited in one render-domain. The viewport FP16 ring remains the startup,
allocation-failure, and transient-miss fallback until the D3D12 source-cache
path is fully promoted.

The retained D3D11 source/Flutter graph is no longer a product path. Diagnostic
fields with `windowsRetainedGraph*` names remain as compatibility evidence and
should report inactive/zero.
Projection performance should be proven through wgpu/D3D12 source consumption,
Flutter surface generation consumption, present cadence, and hot-path
diagnostics rather than DComp retained bake counts.

The source cache budget is 384 MiB. A three-slot bundle ring is preferred; if
that exceeds the budget, one frozen snapshot is allowed. A single bundle that
still exceeds the budget is rejected without failing playback. Signature
changes stop exposing the previous bundle, while leased old generations remain
alive until release. A draw miss with an unchanged signature keeps the last
complete bundle and never publishes partial track updates.

Analysis overlay in source-projection mode is owned by the wgpu/D3D12 composite
pass. The runner no longer rasterizes, uploads, or composites overlay
primitives through D3D11. New overlay features should target the wgpu renderer.
Pan, zoom, split, and order changes must still update projection constants
without rebuilding CPU vertices. Rebuild failure drops only the overlay layer
for that frame and reports a fallback reason. Device removal follows the normal
presentation recovery contract and clears old overlay generations.

The standalone native window path may use a double-buffered flip-discard swap
chain. It is not the current Flutter product presentation route.

## Ownership And Threads

- Shared renderer code owns scheduling, `PresentDecision`,
  `RendererDrawSnapshot`, layout, and normalized color metadata.
- `WgpuD3D12PresentationBackend` / Rust `WgpuRenderCore` owns the target
  composition path: video/source imports, projection, overlay, Flutter surface
  import, color, layout, and final-target rendering.
- `WindowsD3D12PresentTarget` owns swap-chain buffers, the DComp visual, the
  explicit `PRESENT -> RENDER_TARGET -> PRESENT` transitions around external
  Rust rendering, and the final DXGI present.
- `WgpuD3D12SharedFp16Ring` owns renderer-local FP16 fallback buffers used only
  before source-cache/direct-present is ready or after allocation failure.
- `WgpuD3D12SourceCache` owns atomic source-texture bundles, generation
  retirement, the 384 MiB depth policy, and overlay-package attachment.
- `FlutterTextureBridge` owns Flutter texture registration and lease release.
- The engine fork owns immutable Flutter surface leases. Old resize
  generations remain alive until their leases are released. In active
  compositor-owned mode it must keep publishing a full premultiplied-alpha
  surface stream for normal Flutter UI frames; `mirror` is only a preparation
  mode before DComp becomes active, not an active fallback.
- `WindowsNativeCompositor` owns the Windows present bridge: an independent
  D3D/DComp device, target, candidate/current SDR or FP16 swap chains,
  composition thread, and the latest successfully synchronized final-target or
  compatibility input leases. It must not regain ownership of Flutter UI
  blending or final color/layout composition.
  Held inputs are replaced only after a newer generation is acquired
  successfully, then released during replacement, failed-state cleanup, or
  shutdown.
- D3D12 decode/source work is synchronized by producer fences before import.
- The lock order is `device_mutex -> texture_mutex`.
- Host callbacks are invoked after presentation locks are released.

Presentation changes must stay behind `PresentationBackend`. Windows-specific
DXGI, shared-handle, compositor, or color-target policy does not belong in the
shared scheduler.

## Format And Color Contract

- Auto SDR presents `B8G8R8A8_UNORM` in
  `RGB_FULL_G22_NONE_P709`.
- Auto HDR currently falls back to the Auto SDR target and records
  `hdr-ui-composition-unsupported`; forced `native-compositor-scrgb` presents
  `R16G16B16A16_FLOAT`, linear BT.709 scRGB, in
  `RGB_FULL_G10_NONE_P709` for diagnostics.
- The wgpu render core keeps FP16 video/source resources as the common
  compositor input.
- SDR final composition samples the source-rerendered BGRA compatibility
  texture only where the compatibility bridge still requires it. The target
  path is source projection in wgpu/D3D12, where each PQ/HLG source is
  tone-mapped before Flutter premultiplied sRGB source-over.
- scRGB `1.0` represents 80 nits. SDR content, backgrounds, dividers, and
  overlays are linearized and scaled by `SDRWhiteLevel / 80`.
- PQ is decoded to absolute nits then divided by 80. HLG keeps the shared
  reference policy and is converted to the same 80-nit scale.
- Valid FP16 values above `1.0` and below `0.0` are not clamped.
- Flutter export is BGRA premultiplied sRGB. On the forced scRGB diagnostic
  path, the wgpu final composition shader recovers straight RGB, decodes sRGB
  to linear, re-premultiplies, applies `SDRWhiteLevel / 80`, and performs
  standard premultiplied source-over. This is not yet the Windows Auto HDR
  product path because it bypasses system-managed SDR UI composition.
- CPU fallback packages are BGRA byte streams. The Windows upload texture is
  also `B8G8R8A8_UNORM`; treating that storage as RGBA swaps red and blue.
- NV12, planar YUV420, and P010 are sampled by the shared HLSL color pipeline.
- Limited/full range, BT.601/709/2020 matrix, transfer, and primaries metadata
  must remain deterministic across software and hardware paths.
- There is no generic libswscale/libyuv fallback.
- No HDR10 swap chain or `SetHDRMetaData` call is used; DWM maps scRGB to the
  active Advanced Color output.
- Windows display calibration is system-managed through Advanced Color. The
  player records the reported mode, primaries, white point, and SDR white level
  but does not apply custom ICC/LUT correction or subjective display tuning.

## D3D12 Import And State Contract

The Rust wgpu core imports raw D3D12 resources through `wgpu-hal`. Every import
must carry an explicit state contract in `VPWgpuD3D12CompositeRequest`; the Rust
side rejects mismatches before creating a `wgpu::Texture`.

| Resource | Producer / owner | Required state when handed to Rust | State after Rust render |
| --- | --- | --- | --- |
| Direct-present destination | `WindowsD3D12PresentTarget` | `RENDER_TARGET`; the present target transitions the swap-chain buffer from `PRESENT` immediately before Rust render | `RENDER_TARGET`; the present target transitions it back to `PRESENT` before DXGI present |
| Shared FP16/source-cache destination | `WgpuD3D12PresentationBackend` | Slot state tracked by the owning ring: newly allocated textures start in `COMMON`, reused textures re-enter Rust in the last published state (`RENDER_TARGET`) | `RENDER_TARGET`; the owning ring records this only after a successful publish |
| D3D12VA source texture | FFmpeg D3D12VA / renderer frame storage | `COMMON` after the producer fence signals | `COMMON`; Rust samples it as an imported read-only texture |
| Flutter UI surface | locked Flutter engine export | `COMMON` after the export fence signals | `COMMON`; Rust samples it as premultiplied BGRA |

The D3D12 queue exposed by the Rust renderer is the queue used by the present
target. Queue order therefore preserves the `PRESENT -> RENDER_TARGET`
transition before the Rust submit, and the Rust submit before
`RENDER_TARGET -> PRESENT`. `device.poll(PollType::Wait)` remains in the
external-resource path until the present path grows an end-to-end asynchronous
fence handoff; high-refresh gates must prove this wait has not regressed the
hot path before merge.

If a valid Flutter surface exists but its fence is not ready within the bounded
wait, the draw fails and the compositor defers the present. It must not submit
a new video-only frame that silently drops Flutter UI; the previous complete
frame remains visible instead. The direct-present owner transitions the
acquired back buffer back to `PRESENT` without flipping before returning to the
composition loop, so the next acquire starts from a defined state. Missing
Flutter surfaces during startup remain
diagnostic state, but stale or late surfaces must be visible through
`windowsPresentationExternalFlutterSurface*` counters.

The `wgpu_d3d12` native presentation backend tests in
`test_renderer_config_validation.cpp` cover D3D12 render-target import,
premultiplied Flutter BGRA overlay composition, shared FP16 publication,
D3D12VA NV12 sampling, CPU planar YUV420 sampling, and retained source-cache
bundles. The Windows preservation scRGB UI profile covers the real windowed
path through seek preview, Flutter surface pumping, source projection, device
recovery, and the forced scRGB compositor mode.

## Diagnostics Contract

`getDiagnostics` reports the active facts without inferring HDR capability:

| Key | Current meaning |
| --- | --- |
| `windowsPresentationRequest` | `auto`, `sdr`, `native-compositor-sdr`, `native-compositor-scrgb`, or the rejected request |
| `windowsPresentationMode` | `native-compositor-sdr`, `native-compositor-scrgb`, or `native-compositor-failed` |
| `windowsPresentationReason` | Selected policy reason |
| `windowsPresentationAutoEnabled/HasHDRTrack/DesiredMode` | Auto resolver inputs and requested target |
| `windowsPresentationTransition*` | Candidate target state, serial, and reason |
| `windowsPresentationOutputGeneration` | Successfully committed output generation |
| `windowsPresentationHDRPromotionCount/DemotionCount` | Runtime target transitions |
| `windowsPresentationTargetFallbackCount` | HDR candidates that fell back to native SDR |
| `windowsPresentationCrossAdapterRequired` | Current policy requires output-adapter migration |
| `windowsPresentationProducerAdapterLuid/OutputAdapterLuid/PendingOutputAdapterLuid` | Producer, committed output, and pending output adapter identities |
| `windowsPresentationCrossAdapterSupported/Active` | Cross-adapter transport availability and active route |
| `windowsPresentationOutputMigrationCount/OutputMigrationFailureCount` | Runtime output-device migration evidence |
| `windowsPresentationLockedDisplayGeneration/LockedSDRWhiteLevelMilliNits` | Inputs locked to the current target generation |
| `windowsPresentationBackend` | Active backend identity, expected to be `wgpu-d3d12` on the Windows native compositor path |
| `windowsPresentationTargetFormat` | `B8G8R8A8_UNORM` |
| `windowsPresentationRenderTargetFormat/RenderColorSpace` | Actual internal render target and working color space |
| `windowsPresentationFP16Target*` | Active state, dimensions, and single-buffer contract |
| `windowsPresentationSDRCompatibilityPass` | `source-rerender` while FP16 is active |
| `windowsPresentationSDRWhiteLevel*` | Player-creation locked white level/status/scale |
| `windowsPresentationFP16DrawCount` | Successful experimental draws |
| `windowsPresentationSDRCompatibilityDrawCount` | Matching BGRA compatibility redraws |
| `windowsPresentationExternalFlutterSurface*` | D3D12 Flutter surface update/consume/wait evidence for the wgpu composition path |
| `windowsPresentationFallbackReason` | Native target downgrade/failure reason; never a Flutter Texture restore reason |
| `windowsPresentationWidth/Height` | Active target dimensions |
| `windowsPresentationBufferCount` | Active output buffer count |
| `windowsPresentationHeadless` | Whether the backend is using shared textures |
| `windowsNativeCompositorPhase` | `inactive`, `preparing`, `active`, or `failed` |
| `windowsNativeCompositorStateSerial/AckSerial` | Flutter alpha-hole handshake serials |
| `windowsFlutterExportGeneration/windowsVideoRingGeneration` | Latest consumed input generations |
| `windowsFlutterExportFramePumpAvailable` | Locked engine exposes the compositor-owned surface export request/state ABI |
| `windowsFlutterExportPublishCount/windowsFlutterExportRequestCount` | Engine-owned export frame stream activity |
| `windowsFlutterExportStaleTimeoutCount` | Native compositor failed closed because a requested Flutter generation never advanced |
| `windowsDComp*` | Present-bridge swap-chain format/color space/support, SDR tone-map state, and composite/present/drop/failure counters |
| `windowsDeviceRecovery*` | In-place D3D11/DComp recovery state, generation, attempts, success/failure counters, preserved player/track evidence, last removed reason, fallback stage, and last-frame hold |
| `windowsCrossAdapter*` | Transport mode/status, requested/active sync kind, shared-fence capability/open/signal/wait counters, event-query/shared-fence P95 waits, copy counters, consumed generations, fallback reason, and last error |
| `windowsOverlay*` | Retained overlay layer active state, mode, generation, bytes, raster/upload/reuse/composite/miss/backpressure counts, p95 costs, and fallback reason |
| `windowsHotPath*` | Source-projection hot-path summary: active/mode, display Hz, frame budget, present/composite/acquire/input p95, drop rate, source/overlay reuse, viewport redraws, failure reason, and gate result |
| `windowsRetainedGraph*` | Compatibility counters for the removed retained D3D11 source/Flutter graph; product-route evidence should remain inactive/zero |
| `nativeCompositorSource*` | Projection/cache activity, generation, bytes, rates, and overlay primitive counts |
| `windowsSourceCache*` | Format, depth/frozen policy, publish/backpressure/consume/fallback counters |
| `windowsD3DAdapter*` | Description, vendor/device IDs, and LUID |
| `windowsD3DFeatureLevel` | Active D3D feature level |
| `windowsD3DDriverType` / `windowsD3DWarp` | Device creation route |
| `d3dDeviceLost` / `d3dDeviceRemovedReason` | Existing compatibility fields |

The runner also resolves the active DXGI output from the top-level window
rectangle on every diagnostics query. It selects the attached output with the
greatest intersection, then falls back to the nearest monitor or first attached
output. `windowsDisplay*` fields report the selected output, adapter LUID,
desktop geometry, rotation, bits per color, DXGI color space, luminance
metadata, current SDR white level, Advanced Color API/mode, calibration source,
reported primaries/white point, and probe generation/change reason.

`windowsDisplayHDRActive=true` only means DXGI explicitly reported a PQ or HLG
HDR color space. A normal SDR color space is reported as
`sdr-or-advanced-color-unknown`, because `IDXGIOutput6::GetDesc1` cannot
reliably distinguish Windows 11 SDR Advanced Color/WCG from ordinary SDR.

A fallback that changes adapter, driver type, target format, or presentation
mode must update these fields and emit a clear log reason. Windows presentation
must not report `flutter-texture-sdr`.

## Device Loss And Fallback

D3D11 and DComp device-removed/reset/hung errors first enter the Windows
presentation recovery state machine instead of destroying the player. The
renderer marks the presentation device as lost, stops publishing old shared
generations, releases BGRA/FP16/source-cache/shader/device resources, rebuilds
the D3D11 backend from its saved presentation config, clears old source-cache
generations, and requests a current-frame redraw. EOF and sparse-tail stable
display semantics remain renderer-owned and do not change during recovery.

`WindowsNativeCompositor` keeps the last successful DComp frame visible while
the composition thread releases held video/Flutter/source leases, rebuilds the
output D3D/DComp device and candidate SDR/scRGB swap chain, refreshes
cross-adapter transport resources, reacquires a Flutter surface, and waits for
fresh video/source generations before returning to `active`.

Recovery states are diagnostic strings:
`stable`, `device-lost-detected`, `holding-last-frame`,
`rebuilding-presentation`, `waiting-for-fresh-video`,
`reactivating-compositor`, `recovered`, `fallback-native-sdr`,
and `failed-terminal`.

The debug automation hook
`DEBUG_SIMULATE_WINDOWS_DEVICE_LOSS,target,reason` drives the MethodChannel
method `debugSimulateWindowsDeviceLoss`. Valid targets are `presentation`,
`compositor`, `transport`, and `source-cache`. Synthetic injection is gate
evidence; real TDR/device-reset validation is supplemental local evidence.

Auto HDR currently keeps the product route on the native SDR target when the
session contains HDR media. Diagnostics preserve the intended
`windowsPresentationDesiredMode=native-compositor-scrgb`, but report
`windowsPresentationMode=native-compositor-sdr` and
`windowsPresentationFallbackReason=hdr-ui-composition-unsupported`. The forced
`native-compositor-scrgb` mode remains a diagnostic path until Windows can keep
Flutter's SDR UI as a system-managed surface while presenting HDR video/source
content. HDR target creation or Present failure first attempts the native SDR
target. Unknown requests, missing engine export, or SDR compositor failure fail
closed and expose the selected request, actual mode, transition, and failure
reason.

## High Refresh Interaction Diagnostics

Windows native compositor exposes high-refresh parity counters for the
source-projection path while it is migrating to wgpu-owned composition.
`RESET_NATIVE_PERF_COUNTERS`,
`BEGIN_NATIVE_INTERACTION_SAMPLE,label`, and
`END_NATIVE_INTERACTION_SAMPLE,label` let UI automation bracket pan/zoom/split
or overlay interactions. Diagnostics include DComp present/composite P95,
acquire-wait P95, input-to-present P95, drop rate, source projection reuse,
viewport redraws during projection, and overlay raster/upload/reuse/composite
cost. Displays below 100 Hz run as `functional-only-low-refresh` evidence;
high-refresh local gates enforce timing thresholds.

During source-cache projection, pan/zoom/split/order changes must update
projection state without forcing viewport-sized renderer redraws. Analysis
overlay diagnostics require the retained bridge, or the future wgpu overlay
path, to reuse GPU-side primitive data so dirty raster/upload does not dominate
high-refresh interaction. The old per-composite CPU vertex rebuild path is not
a valid high-refresh overlay result.

The hot-path gate reports concrete failure reasons:
`fail-viewport-redraw`, `fail-source-cache-no-reuse`,
`fail-overlay-no-reuse`, `fail-present-cadence`, `fail-input-latency`, and
`fail-drop-rate`. `windowsHotPath*` fields are the preferred UI/manual summary;
the older `windowsDComp*`, `windowsSource*`, and `windowsOverlay*` fields remain
the detailed evidence source.

## macOS Parity Mapping

Windows does not copy Metal implementation details, but the product contract now
maps to macOS by behavior:

| Capability | Windows evidence | macOS evidence |
| --- | --- | --- |
| Flutter UI composition | Exported D3D12 premultiplied surface + wgpu/D3D12 final composite | Exported Flutter surface + native compositor |
| Source-resolution interaction | `nativeCompositorSourceCacheActive`, `windowsHotPathSourceCacheReuseCount` | `sourceFrameCacheHitCount`, source projection diagnostics |
| Presented-frame anchor | `nativeCompositorPresentedAnchorMode=source-cache-publish`, source-cache anchor generation fields | source projection publish frame anchor |
| No viewport redraw on pan/zoom | `windowsHotPathViewportRedrawDuringProjectionCount == 0` | display-link viewport composite without renderer round-trip |
| Retained overlay | `windowsOverlayRetainedLayerActive`, reuse/raster counters | retained Metal overlay layers |
| Cadence/latency | `windowsHotPathPresentIntervalP95Us`, `windowsHotPathInputToPresentP95Us` | display-link cadence and renderer-owned presentation counters |
| Fallback visibility | `windowsHotPathLastFailureReason`, presentation fallback fields | native compositor / renderer-owned fallback diagnostics |

## Catch-Up Roadmap

1. Move the retained overlay primitive bridge into the wgpu/D3D12 composite
   pass while preserving high-refresh reuse diagnostics.
2. Thin `WindowsNativeCompositor` to final-target acquisition, DComp/DXGI
   present, HDR/SDR target switching, and device-loss recovery.
3. Harden Windows release evidence: real HDR, multi-adapter, and high-refresh
   combinations must preserve native compositor state, source projection,
   overlay reuse, and documented fallback diagnostics.

This sequence is capability parity with macOS, not a mechanical Metal/Swift
port.
