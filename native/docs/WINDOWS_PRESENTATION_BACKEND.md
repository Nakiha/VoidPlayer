# Windows Presentation Backend

This document defines the Windows product presentation contract. D3D11
implementation details remain in [D3D11_BACKEND.md](D3D11_BACKEND.md); shared
renderer ownership and color rules remain in
[ARCHITECTURE.md](ARCHITECTURE.md) and [COLOR_PIPELINE.md](COLOR_PIPELINE.md).

## Current Product And Experimental Routes

The default Windows product policy is now `auto` and requires the locked
VoidPlayer Flutter engine:

```text
shared RendererDrawSnapshot
  -> D3D11RenderBackend
  -> shared FP16 video/source rings + BGRA compatibility ring
  -> WindowsNativeCompositor
  -> BGRA8 SDR or FP16 scRGB DComp swap chain
  -> Flutter HWND
```

SDR-only sessions use `native-compositor-sdr`. A session containing an active
PQ/HLG track promotes to `native-compositor-scrgb` when the selected DXGI output
explicitly reports HDR. Matching adapters stay on the producer device. Adapter
mismatch requests an output-device migration; if cross-adapter BGRA transport is
not available the policy falls back to native SDR, and if FP16 transport is not
available the migrated output also stays SDR. The runner obtains Flutter's DXGI
adapter and the resolved output adapter and passes both into the native
compositor. Hosted CI may
explicitly enable
`VOIDPLAYER_ALLOW_D3D11_HEADLESS_WARP_FALLBACK=1`; that is launch/contract
coverage, not release evidence for a desktop GPU.

The compatibility request `sdr` is now an alias for `native-compositor-sdr`.
`native-compositor-sdr` and `native-compositor-scrgb` remain explicit
diagnostic modes; forced scRGB does not depend on the current Windows HDR
toggle. `flutter-texture-sdr`, `flutter`, `fp16-scrgb`, and unknown Windows
presentation requests fail closed instead of creating a Flutter Texture video
route.

The forced HDR compositor diagnostic mode is:

```powershell
$env:VOIDPLAYER_WINDOWS_PRESENTATION_MODE="native-compositor-scrgb"
python dev.py launch
```

The Auto and forced native-compositor routes require the locked VoidPlayer
Flutter local engine. ANGLE copies
Flutter's final premultiplied-alpha frame into a three-slot shared BGRA ring.
The renderer publishes a separate three-slot shared FP16 scRGB video ring.
`WindowsNativeCompositor` consumes both rings on its composition thread and
presents one full-window flip swap chain through DirectComposition attached
directly to the Flutter HWND. The SDR target is `B8G8R8A8_UNORM` with
`RGB_FULL_G22_NONE_P709`; the HDR target is `R16G16B16A16_FLOAT` with
`RGB_FULL_G10_NONE_P709`. It does not use a key color, `WS_EX_LAYERED`, window
capture, a child HWND sandwich, or a rectangular native hole.

Runtime track changes and debounced display/window notifications rerun the same
resolver. Target changes build and render a candidate swap chain from retained
video/source/Flutter inputs, Present it, then atomically replace the DComp
visual content. HDR candidate failure falls back to native SDR; failure of the
SDR compositor enters an explicit failed state and does not restore Flutter
Texture SDR.

If the active output belongs to a different adapter, the renderer and Flutter
export remain on the producer adapter while `WindowsNativeCompositor` recreates
its D3D/DComp device on the output adapter. `D3D11CrossAdapterTextureTransport`
bridges immutable input leases through row-major shared NT-handle textures and
GPU copies into output-local SRVs. The default synchronization path waits on a
producer event query. `VOIDPLAYER_WINDOWS_CROSS_ADAPTER_SYNC=shared-fence`
requests the optional shared-fence path for local evidence; initialization or
capability failure falls back to event-query with diagnostics. The transport
never reads pixels back to the CPU and does not change color transforms.

Once Dart publishes a valid projection signature, D3D11 also maintains an
atomic source-resolution bundle with up to four FP16 scRGB textures. The render
thread draws every active source from the same prepared frame snapshot, then
publishes the complete bundle with its analysis primitive package. DComp
applies pan, zoom, side-by-side order, split position, background, and retained
overlay projection without waiting for a viewport-sized redraw. The viewport
FP16 ring remains the startup, allocation-failure, and transient-miss fallback.

On same-adapter SDR and scRGB targets, `WindowsNativeCompositor` can switch the
active DComp root to a retained visual graph after the first normal compositor
present and transparent Flutter ACK. Source-cache and Flutter-export content
updates are baked into retained `IDCompositionSurface` layers (`BGRA8` for SDR,
`R16G16B16A16_FLOAT` for scRGB). Pure projection changes then update only the
source visual transform/clip and commit the DComp tree, so pan/zoom/split/order
interactions do not call final swap-chain `Present` and should report
`windowsDCompPresentBlockP95Us == 0`. Cross-adapter output keeps using the
full-composite path until there is separate local transport evidence.

The source cache budget is 384 MiB. A three-slot bundle ring is preferred; if
that exceeds the budget, one frozen snapshot is allowed. A single bundle that
still exceeds the budget is rejected without failing playback. Signature
changes stop exposing the previous bundle, while leased old generations remain
alive until release. A draw miss with an unchanged signature keeps the last
complete bundle and never publishes partial track updates.

Analysis overlay in source-projection mode is retained on the DComp compositor
device. A dirty primitive package is packed once into a video-space GPU
primitive buffer keyed by primitive generation, track/file/size/mode signature,
output target class, and SDR white scale. Pan, zoom, split, and order changes
only update projection constants; they do not rebuild or upload the overlay
buffer. Rebuild failure drops only the overlay layer for that frame and reports
a fallback reason. Device removal follows the normal presentation recovery
contract and clears old overlay generations.

The standalone native window path may use a double-buffered flip-discard swap
chain. It is not the current Flutter product presentation route.

## Ownership And Threads

- Shared renderer code owns scheduling, `PresentDecision`,
  `RendererDrawSnapshot`, layout, and normalized color metadata.
- `D3D11RenderBackend` owns the D3D11 device-facing presentation resources.
- `D3D11HeadlessOutput` owns the shared BGRA texture ring, handles, front/back
  selection, GPU fence, and capture.
- `D3D11Fp16Target` owns the single-buffer renderer-only scRGB texture, RTV,
  SRV, and test capture.
- `D3D11SharedFp16Ring` owns the keyed-mutex FP16 video leases used only by
  `native-compositor-scrgb`.
- `D3D11SharedSourceCacheRing` owns atomic source-texture bundles, generation
  retirement, the 384 MiB depth policy, and overlay-package attachment.
- `D3D11CrossAdapterTextureTransport` owns row-major shared bridge textures,
  producer-to-bridge and bridge-to-output GPU copies, capability reporting, and
  copy/backpressure diagnostics for cross-adapter compositor inputs.
- `FlutterTextureBridge` owns Flutter texture registration and lease release.
- The engine fork owns immutable Flutter surface leases. Old resize
  generations remain alive until their leases are released. In active
  compositor-owned mode it must keep publishing a full premultiplied-alpha
  surface stream for normal Flutter UI frames; `mirror` is only a preparation
  mode before DComp becomes active, not an active fallback.
- `WindowsNativeCompositor` owns an independent D3D11 device/context, DComp
  target, candidate/current SDR or FP16 swap chains, final shader, composition
  thread, and the latest
  successfully synchronized video, Flutter, source-cache input leases, and
  retained overlay primitive buffers.
  Held inputs are replaced only after a newer generation is acquired
  successfully, then released during replacement, failed-state cleanup, or
  shutdown.
- D3D11 immediate-context work is serialized by the presentation device mutex.
- The lock order is `device_mutex -> texture_mutex`.
- Host callbacks are invoked after presentation locks are released.

Presentation changes must stay behind `PresentationBackend`. Windows-specific
DXGI, shared-handle, compositor, or color-target policy does not belong in the
shared scheduler.

## Format And Color Contract

- Auto SDR presents `B8G8R8A8_UNORM` in
  `RGB_FULL_G22_NONE_P709`.
- Auto HDR presents `R16G16B16A16_FLOAT`, linear BT.709 scRGB, in
  `RGB_FULL_G10_NONE_P709`.
- The renderer keeps FP16 video/source rings as the common compositor input.
- SDR final composition samples the existing source-rerendered BGRA
  compatibility texture, while source projection tone-maps each PQ/HLG source
  before Flutter premultiplied sRGB source-over.
- scRGB `1.0` represents 80 nits. SDR content, backgrounds, dividers, and
  overlays are linearized and scaled by `SDRWhiteLevel / 80`.
- PQ is decoded to absolute nits then divided by 80. HLG keeps the shared
  reference policy and is converted to the same 80-nit scale.
- Valid FP16 values above `1.0` and below `0.0` are not clamped.
- Flutter export is BGRA premultiplied sRGB. The final shader recovers straight
  RGB, decodes sRGB to linear, re-premultiplies, applies
  `SDRWhiteLevel / 80`, and performs standard premultiplied source-over.
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

`windows_d3d11_color_layout_parity_smoke` captures real D3D11 output and checks
BGRA channel order, NV12, planar YUV420, P010, odd dimensions/padded stride,
aspect fit/background bars, and split/order behavior against CPU expectations.
`windows_d3d11_fp16_scrgb_smoke` additionally captures RGBA16F output and
checks SDR 80/203-nit scaling, PQ/HLG, P010, BT.2020 to BT.709 conversion,
unclipped highlights, overlay-pass participation, odd/padded input, layout,
and unchanged BGRA compatibility output.

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
| `windowsPresentationBackend` | `d3d11` |
| `windowsPresentationTargetFormat` | `B8G8R8A8_UNORM` |
| `windowsPresentationRenderTargetFormat/RenderColorSpace` | Actual internal render target and working color space |
| `windowsPresentationFP16Target*` | Active state, dimensions, and single-buffer contract |
| `windowsPresentationSDRCompatibilityPass` | `source-rerender` while FP16 is active |
| `windowsPresentationSDRWhiteLevel*` | Player-creation locked white level/status/scale |
| `windowsPresentationFP16DrawCount` | Successful experimental draws |
| `windowsPresentationSDRCompatibilityDrawCount` | Matching BGRA compatibility redraws |
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
| `windowsDComp*` | Swap-chain format/color space/support, SDR tone-map state, and composite/present/drop/failure counters |
| `windowsDeviceRecovery*` | In-place D3D11/DComp recovery state, generation, attempts, success/failure counters, preserved player/track evidence, last removed reason, fallback stage, and last-frame hold |
| `windowsCrossAdapter*` | Transport mode/status, requested/active sync kind, shared-fence capability/open/signal/wait counters, event-query/shared-fence P95 waits, copy counters, consumed generations, fallback reason, and last error |
| `windowsOverlay*` | Retained overlay layer active state, mode, generation, bytes, raster/upload/reuse/composite/miss/backpressure counts, p95 costs, and fallback reason |
| `windowsHotPath*` | Source-projection hot-path summary: active/mode, display Hz, frame budget, present/composite/acquire/input p95, drop rate, source/overlay reuse, viewport redraws, failure reason, and gate result |
| `windowsRetainedGraph*` | DComp retained source/Flutter graph state, fallback reason, commit counts, source/Flutter bake counts, and projection-only commits that skipped final swap-chain Present |
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

HDR target creation or Present failure first attempts the native SDR target.
Unknown requests, missing engine export, or SDR compositor failure fail closed
and expose the selected request, actual mode, transition, and failure reason.

## High Refresh Interaction Diagnostics

Windows native compositor exposes high-refresh parity counters for the retained
source-projection path. `RESET_NATIVE_PERF_COUNTERS`,
`BEGIN_NATIVE_INTERACTION_SAMPLE,label`, and
`END_NATIVE_INTERACTION_SAMPLE,label` let UI automation bracket pan/zoom/split
or overlay interactions. Diagnostics include DComp present/composite P95,
acquire-wait P95, input-to-present P95, drop rate, source projection reuse,
viewport redraws during projection, and overlay raster/upload/reuse/composite
cost. Displays below 100 Hz run as `functional-only-low-refresh` evidence;
high-refresh local gates enforce timing thresholds.

During source-cache projection, pan/zoom/split/order changes must update
compositor projection state without forcing viewport-sized renderer redraws.
Analysis overlay diagnostics require retained layer reuse to outpace dirty
raster/upload on high-refresh displays. The old per-composite CPU vertex rebuild
path is not a valid high-refresh overlay result.

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
| Flutter UI composition | Exported BGRA premultiplied surface + DComp final composite | Exported Flutter surface + native compositor |
| Source-resolution interaction | `nativeCompositorSourceCacheActive`, `windowsHotPathSourceCacheReuseCount` | `sourceFrameCacheHitCount`, source projection diagnostics |
| Presented-frame anchor | `nativeCompositorPresentedAnchorMode=source-cache-publish`, source-cache anchor generation fields | source projection publish frame anchor |
| No viewport redraw on pan/zoom | `windowsHotPathViewportRedrawDuringProjectionCount == 0` | display-link viewport composite without renderer round-trip |
| Retained overlay | `windowsOverlayRetainedLayerActive`, reuse/raster counters | retained Metal overlay layers |
| Cadence/latency | `windowsHotPathPresentIntervalP95Us`, `windowsHotPathInputToPresentP95Us` | display-link cadence and renderer-owned presentation counters |
| Fallback visibility | `windowsHotPathLastFailureReason`, presentation fallback fields | native compositor / renderer-owned fallback diagnostics |

## Catch-Up Roadmap

1. Harden Windows release evidence: real HDR, multi-adapter, and high-refresh
   combinations must preserve native compositor state, source projection,
   retained overlay reuse, and documented fallback diagnostics.

This sequence is capability parity with macOS, not a mechanical Metal/Swift
port.
