# Windows Presentation Backend

This document defines the Windows product presentation contract. D3D11
implementation details remain in [D3D11_BACKEND.md](D3D11_BACKEND.md); shared
renderer ownership and color rules remain in
[ARCHITECTURE.md](ARCHITECTURE.md) and [COLOR_PIPELINE.md](COLOR_PIPELINE.md).

## Current Product Route

The Flutter Windows runner currently uses one fixed SDR route:

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

The standalone native window path may use a double-buffered flip-discard swap
chain. It is not the current Flutter product presentation route.

## Ownership And Threads

- Shared renderer code owns scheduling, `PresentDecision`,
  `RendererDrawSnapshot`, layout, and normalized color metadata.
- `D3D11RenderBackend` owns the D3D11 device-facing presentation resources.
- `D3D11HeadlessOutput` owns the shared BGRA texture ring, handles, front/back
  selection, GPU fence, and capture.
- `FlutterTextureBridge` owns Flutter texture registration and lease release.
- D3D11 immediate-context work is serialized by the presentation device mutex.
- The lock order is `device_mutex -> texture_mutex`.
- Host callbacks are invoked after presentation locks are released.

Presentation changes must stay behind `PresentationBackend`. Windows-specific
DXGI, shared-handle, compositor, or color-target policy does not belong in the
shared scheduler.

## Format And Color Contract

- The current presentation target is SDR `B8G8R8A8_UNORM`.
- CPU fallback packages are BGRA byte streams. The Windows upload texture is
  also `B8G8R8A8_UNORM`; treating that storage as RGBA swaps red and blue.
- NV12, planar YUV420, and P010 are sampled by the shared HLSL color pipeline.
- Limited/full range, BT.601/709/2020 matrix, transfer, and primaries metadata
  must remain deterministic across software and hardware paths.
- There is no generic libswscale/libyuv fallback.

`windows_d3d11_color_layout_parity_smoke` captures real D3D11 output and checks
BGRA channel order, NV12, planar YUV420, P010, odd dimensions/padded stride,
aspect fit/background bars, and split/order behavior against CPU expectations.

## Diagnostics Contract

`getDiagnostics` reports the active facts without inferring HDR capability:

| Key | Current meaning |
| --- | --- |
| `windowsPresentationRequest` | `sdr` |
| `windowsPresentationMode` | `flutter-texture-sdr` for the runner |
| `windowsPresentationReason` | `fixed-sdr-current-route` |
| `windowsPresentationBackend` | `d3d11` |
| `windowsPresentationTargetFormat` | `B8G8R8A8_UNORM` |
| `windowsPresentationWidth/Height` | Active target dimensions |
| `windowsPresentationBufferCount` | Active output buffer count |
| `windowsPresentationHeadless` | Whether the backend is using shared textures |
| `windowsPresentationCompositorActive` | `false` until a native compositor ships |
| `windowsD3DAdapter*` | Description, vendor/device IDs, and LUID |
| `windowsD3DFeatureLevel` | Active D3D feature level |
| `windowsD3DDriverType` / `windowsD3DWarp` | Device creation route |
| `d3dDeviceLost` / `d3dDeviceRemovedReason` | Existing compatibility fields |

The runner also resolves the active DXGI output from the top-level window
rectangle on every diagnostics query. It selects the attached output with the
greatest intersection, then falls back to the nearest monitor or first attached
output. `windowsDisplay*` fields report the selected output, adapter LUID,
desktop geometry, rotation, bits per color, DXGI color space, luminance
metadata, and probe generation/change reason.

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

The product path must not silently downgrade presentation quality. Experimental
routes must be explicitly gated, must preserve the fixed SDR route as fallback,
and must expose the selected mode and reason.

## Catch-Up Roadmap

1. Add an experimental FP16/scRGB target behind an explicit opt-in.
2. Productize one native DirectComposition topology with clear Flutter overlay
   ownership and fallback.
3. Add Windows source projection/cache behavior at the backend boundary.
4. Add HDR Auto policy only after output probing, FP16, compositor ownership,
   diagnostics, and preservation gates are stable.

This sequence is capability parity with macOS, not a mechanical Metal/Swift
port.
