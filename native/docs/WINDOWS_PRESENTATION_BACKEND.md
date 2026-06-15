# Windows Presentation Backend

This document defines the Windows product presentation contract. D3D11
implementation details remain in [D3D11_BACKEND.md](D3D11_BACKEND.md); shared
renderer ownership and color rules remain in
[ARCHITECTURE.md](ARCHITECTURE.md) and [COLOR_PIPELINE.md](COLOR_PIPELINE.md).

## Current Product And Experimental Routes

The default Flutter Windows runner route remains:

```text
shared RendererDrawSnapshot
  -> D3D11RenderBackend
  -> triple-buffered DXGI_FORMAT_B8G8R8A8_UNORM shared textures
  -> FlutterTextureBridge
  -> Flutter Texture widget
```

The active mode is `flutter-texture-sdr`. The runner obtains Flutter's DXGI
adapter and passes it into the native renderer so shared handles remain on the
same adapter family. The backend does not silently create a different hardware
device. Hosted CI may explicitly enable
`VOIDPLAYER_ALLOW_D3D11_HEADLESS_WARP_FALLBACK=1`; that is launch/contract
coverage, not release evidence for a desktop GPU.

An internal opt-in creates an additional renderer-owned FP16 target:

```powershell
$env:VOIDPLAYER_WINDOWS_PRESENTATION_MODE="fp16-scrgb"
python dev.py launch
```

```text
same RendererDrawSnapshot
  -> R16G16B16A16_FLOAT scRGB composition pass
  -> source-rerender compatibility pass
  -> existing triple-buffered B8G8R8A8_UNORM Flutter texture
```

The mode is `flutter-texture-sdr-fp16-scrgb`. The FP16 target is not published
to Flutter and does not claim HDR presentation. Initialization, resize, or
non-device-loss draw failure disables only the experimental pass and records a
fallback reason; BGRA playback continues.

The productization opt-in is:

```powershell
$env:VOIDPLAYER_WINDOWS_PRESENTATION_MODE="native-compositor-scrgb"
python dev.py launch
```

This route requires the locked VoidPlayer Flutter local engine. ANGLE copies
Flutter's final premultiplied-alpha frame into a three-slot shared BGRA ring.
The renderer publishes a separate three-slot shared FP16 scRGB video ring.
`WindowsNativeCompositor` consumes both rings on its composition thread and
presents one full-window `R16G16B16A16_FLOAT` flip swap chain through
DirectComposition attached directly to the Flutter HWND. It does not use a key
color, `WS_EX_LAYERED`, window capture, a child HWND sandwich, or a rectangular
native hole.

Once Dart publishes a valid projection signature, D3D11 also maintains an
atomic source-resolution bundle with up to four FP16 scRGB textures. The render
thread draws every active source from the same prepared frame snapshot, then
publishes the complete bundle with its analysis primitive package. DComp
applies pan, zoom, side-by-side order, split position, background, and overlay
projection without waiting for a viewport-sized redraw. The viewport FP16 ring
remains the startup, allocation-failure, and transient-miss fallback.

The source cache budget is 384 MiB. A three-slot bundle ring is preferred; if
that exceeds the budget, one frozen snapshot is allowed. A single bundle that
still exceeds the budget is rejected without failing playback. Signature
changes stop exposing the previous bundle, while leased old generations remain
alive until release. A draw miss with an unchanged signature keeps the last
complete bundle and never publishes partial track updates.

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
- `FlutterTextureBridge` owns Flutter texture registration and lease release.
- The engine fork owns immutable Flutter surface leases. Old resize
  generations remain alive until their leases are released.
- `WindowsNativeCompositor` owns an independent D3D11 device/context, DComp
  target, FP16 swap chain, final shader, composition thread, and the latest
  successfully synchronized video, Flutter, and source-cache input leases.
  Held inputs are replaced only after a newer generation is acquired
  successfully, then released during replacement, fallback completion, or
  shutdown.
- D3D11 immediate-context work is serialized by the presentation device mutex.
- The lock order is `device_mutex -> texture_mutex`.
- Host callbacks are invoked after presentation locks are released.

Presentation changes must stay behind `PresentationBackend`. Windows-specific
DXGI, shared-handle, compositor, or color-target policy does not belong in the
shared scheduler.

## Format And Color Contract

- The current presentation target is SDR `B8G8R8A8_UNORM`.
- The optional render target is `R16G16B16A16_FLOAT`, linear BT.709 scRGB.
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
| `windowsPresentationRequest` | `sdr`, `fp16-scrgb`, `native-compositor-scrgb`, or the rejected request |
| `windowsPresentationMode` | Actual SDR route or active FP16 experiment |
| `windowsPresentationReason` | Selected policy reason |
| `windowsPresentationBackend` | `d3d11` |
| `windowsPresentationTargetFormat` | `B8G8R8A8_UNORM` |
| `windowsPresentationRenderTargetFormat/RenderColorSpace` | Actual internal render target and working color space |
| `windowsPresentationFP16Target*` | Active state, dimensions, and single-buffer contract |
| `windowsPresentationSDRCompatibilityPass` | `source-rerender` while FP16 is active |
| `windowsPresentationSDRWhiteLevel*` | Player-creation locked white level/status/scale |
| `windowsPresentationFP16DrawCount` | Successful experimental draws |
| `windowsPresentationSDRCompatibilityDrawCount` | Matching BGRA compatibility redraws |
| `windowsPresentationFallbackReason` | Unsupported request or FP16 runtime fallback |
| `windowsPresentationWidth/Height` | Active target dimensions |
| `windowsPresentationBufferCount` | Active output buffer count |
| `windowsPresentationHeadless` | Whether the backend is using shared textures |
| `windowsNativeCompositorPhase` | `inactive`, `preparing`, `active`, or `fallback-restoring` |
| `windowsNativeCompositorStateSerial/AckSerial` | Flutter alpha-hole handshake serials |
| `windowsFlutterExportGeneration/windowsVideoRingGeneration` | Latest consumed input generations |
| `windowsDComp*` | Swap-chain facts and composite/present/drop/failure counters |
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
metadata, current SDR white level, and probe generation/change reason.

`windowsDisplayHDRActive=true` only means DXGI explicitly reported a PQ or HLG
HDR color space. A normal SDR color space is reported as
`sdr-or-advanced-color-unknown`, because `IDXGIOutput6::GetDesc1` cannot
reliably distinguish Windows 11 SDR Advanced Color/WCG from ordinary SDR.

A fallback that changes adapter, driver type, target format, or presentation
mode must update these fields and emit a clear log reason.

## Device Loss And Fallback

Device-removed/reset/hung errors enter the renderer terminal device state and
remain visible through the compatibility diagnostics. Recovery is not yet an
in-place device rebuild.

The product path must not silently downgrade presentation quality. Unknown
presentation requests and FP16 failures preserve the fixed SDR route and expose
the selected request, actual mode, and fallback reason.

## Catch-Up Roadmap

1. Add Windows HDR Auto policy after output probing, FP16, compositor
   ownership, source projection, diagnostics, and preservation gates are stable.

This sequence is capability parity with macOS, not a mechanical Metal/Swift
port.
