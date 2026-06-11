# macOS HDR Exploration

This document tracks the current VoidPlayer macOS HDR/EDR spike. The goal is to
keep the native-composited route reproducible while it is still gated from the
normal release path.

## Flutter Fork Pin

The spike depends on a local Flutter engine patch that exposes the current
macOS Flutter surface texture to the runner.

| Item | Value |
| --- | --- |
| Flutter baseline | `3.44.1` stable |
| Local fork | `/Users/zhuhongwei/Documents/yorune/VoidPlayer-Flutter` |
| Engine patch branch | `voidplayer/hdr-surface-export-3.44.1` |
| Engine patch commit | `04b75e628e3 Expose macOS Flutter surface info for VoidPlayer HDR spike` |
| App worktree | `/Users/zhuhongwei/Documents/yorune/VoidPlayer.worktrees/hdr-support-exploration` |

Local app builds should use `dev_config.local.json` to point at the fork and
local engine:

```json
{
  "flutter": {
    "executable": "/Users/zhuhongwei/Documents/yorune/VoidPlayer-Flutter/bin/flutter",
    "localEngineSrcPath": "/Users/zhuhongwei/Documents/yorune/VoidPlayer-Flutter/engine/src",
    "localEngine": "host_debug_unopt_arm64",
    "localEngineHost": "host_debug_unopt_arm64"
  }
}
```

`dev_config.local.json` is intentionally ignored by git.

## Presentation Modes

The normal app path remains Flutter texture presentation. The spike is selected
with `VOIDPLAYER_MACOS_PRESENTATION_MODE`:

| Mode | Behavior |
| --- | --- |
| unset / `flutter-texture-sdr` | Existing FlutterTexture / BGRA path. |
| `native-compositor-sdr` | Native window compositor overlays the stolen Flutter surface over a BGRA native video target. |
| `native-compositor-edr` | Native compositor uses `CAMetalLayer.rgba16Float`, `wantsExtendedDynamicRangeContent`, and an RGBA half renderer-owned target. |

The old spike variables `VOIDPLAYER_NATIVE_COMPOSITOR_SPIKE`,
`VOIDPLAYER_NATIVE_COMPOSITOR_EDR`, and `VOIDPLAYER_FLUTTER_HDR_SPIKE` remain
accepted as compatibility aliases during the exploration.

## What Is Verified

The current evidence proves the pipeline shape, not final visual calibration:

- Flutter engine patch exposes a Metal texture and IOSurface for the presented
  Flutter surface.
- The app can leave a transparent viewport hole while keeping controls and side
  panels opaque.
- The macOS runner can composite a native video texture and the Flutter alpha
  texture in a native `CAMetalLayer`.
- The EDR route allocates a renderer-owned `kCVPixelFormatType_64RGBAHalf`
  target and presents through `MTLPixelFormatRGBA16Float`.
- `DEBUG_NATIVE_COMPOSITOR_SPIKE` reports direct half-float video target
  diagnostics:
  - `nativeCompositorEDRVideoMaxRGBX1000`
  - `nativeCompositorEDRVideoPixelsOver1X1000`
  - `macOSDisplayEDRHeadroomX1000`

Screenshots remain useful for "not black / UI overlay exists" checks, but they
are not HDR luminance proof because macOS can tone-map screenshots.

## Local Validation

Default path:

```bash
python dev.py mac-ui-test --build ui_tests/macos/native_facade_smoke.csv
```

SDR content through EDR compositor:

```bash
VOIDPLAYER_MACOS_PRESENTATION_MODE=native-compositor-edr \
  python dev.py mac-ui-test ui_tests/macos/native_compositor_flutter_overlay_spike.csv
```

Local Dolby/HLG sample:

```bash
VOIDPLAYER_MACOS_PRESENTATION_MODE=native-compositor-edr \
  python dev.py mac-ui-test build/tmp/dolby_hlg_edr_compositor.csv
```

The local Dolby/HLG CSV references:

```text
/Users/zhuhongwei/Desktop/VID_20260605_212405_DOLBY.mp4
```

That file is not a portable repository fixture.

## Remaining Work

- Replace spike naming with a product-facing presentation capability and user
  preference once the path is ready to expose.
- Add automated assertions for `nativeCompositorEDRVideoMaxRGBX1000 > 1000`
  on a portable HDR fixture.
- Calibrate HLG/PQ mapping against display EDR headroom instead of using the
  current simple spike constants.
- Add proper Display P3 / Rec.2020 gamut mapping.
- Treat Dolby Vision RPU metadata as later color-quality work; the current
  path uses the base HLG/PQ layer.
- Push and protect the Flutter fork branch before any app PR depends on it.
