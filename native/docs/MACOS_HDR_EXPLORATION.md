# macOS HDR Exploration

This document tracks the current VoidPlayer macOS HDR/EDR path. The product
direction is now a default Auto policy rather than an opt-in environment-only
experiment.

## Flutter Fork Pin

The HDR compositor depends on a local Flutter engine patch that exposes the current
macOS Flutter surface texture to the runner.

| Item | Value |
| --- | --- |
| Flutter baseline | `3.44.1` stable |
| Toolchain lock | `toolchains/flutter.lock.json` |
| Fork repo | `https://github.com/Nakiha/VoidPlayer-Flutter.git` |
| Fork release ref | `voidplayer-flutter-3.44.1-hdr.1` |
| Fork patch branch | `voidplayer/hdr-surface-export-3.44.1` |
| Fork commit | `04b75e628e3a7c7ffc66f14e50f760564ab2e9f2` |
| Patch inventory | `toolchains/FLUTTER_FORK_PATCHES.md` |
| App worktree | `/Users/zhuhongwei/Documents/yorune/VoidPlayer.worktrees/hdr-support-exploration` |

All `dev.py` commands that invoke Flutter validate the active SDK against
`toolchains/flutter.lock.json` before running `flutter`. This check verifies the
framework revision, engine revision, Dart SDK version, git checkout revision, a
clean fork working tree, and VoidPlayer patch markers.

The `flutterVersion` string reported by Flutter is informational for fork
checkouts. The revision fields above are the stable lock.

Use:

```bash
python dev.py toolchain doctor
python dev.py toolchain bootstrap-flutter
```

`dev_config.local.json` may point at an existing fork checkout and local engine
build, but it does not replace the lock:

```json
{
  "env": {
    "VOIDPLAYER_FLUTTER_BIN": "/Users/zhuhongwei/Documents/yorune/VoidPlayer-Flutter/bin/flutter",
    "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH": "/Users/zhuhongwei/Documents/yorune/VoidPlayer-Flutter/engine/src",
    "VOIDPLAYER_FLUTTER_LOCAL_ENGINE": "host_debug_unopt_arm64",
    "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST": "host_debug_unopt_arm64",
    "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE": "host_release_arm64",
    "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE": "host_release_arm64"
  }
}
```

`dev_config.local.json` is intentionally ignored by git. If the configured
checkout drifts from the lock, the build fails before producing a VoidPlayer
binary.

## Presentation Policy

macOS defaults to `VOIDPLAYER_MACOS_PRESENTATION_MODE=auto`. Auto uses the
native compositor but keeps SDR media on the SDR BGRA target, so simply opening
an SDR video does not create an EDR layer or reserve display headroom. PQ/HLG
tracks are promoted to the EDR compositor when the display reports potential EDR
headroom.

| Request | Behavior |
| --- | --- |
| unset / `auto` | SDR media -> `native-compositor-sdr`; PQ/HLG media on an EDR-capable display -> `native-compositor-edr`. |
| `flutter-texture-sdr` | Existing FlutterTexture / BGRA path for compatibility and bisecting. |
| `native-compositor-sdr` | Force native window compositor with a BGRA native video target. |
| `native-compositor-edr` | Force `CAMetalLayer.rgba16Float`, `wantsExtendedDynamicRangeContent`, and an RGBA half renderer-owned target when EDR headroom is available. |

The old spike variables `VOIDPLAYER_NATIVE_COMPOSITOR_SPIKE`,
`VOIDPLAYER_NATIVE_COMPOSITOR_EDR`, and `VOIDPLAYER_FLUTTER_HDR_SPIKE` remain
accepted as compatibility aliases. Prefer `VOIDPLAYER_MACOS_PRESENTATION_MODE`
or `VOIDPLAYER_NATIVE_COMPOSITOR=1` for new local runs.

Diagnostics expose the decision:

- `macOSPresentationRequest`
- `macOSPresentationReason`
- `macOSPresentationMode`
- `macOSPresentationEDROutputEnabled`
- `macOSDisplayEDRHeadroomX1000`

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
- `DEBUG_NATIVE_COMPOSITOR` reports direct half-float video target
  diagnostics:
  - `nativeCompositorEDRVideoMaxRGBX1000`
  - `nativeCompositorEDRVideoPixelsOver1X1000`
  - `macOSDisplayEDRHeadroomX1000`

Screenshots remain useful for "not black / UI overlay exists" checks, but they
are not HDR luminance proof because macOS can tone-map screenshots.

## Local Validation

Default SDR Auto policy:

```bash
python dev.py gate macos-ui-smoke
```

The smoke gate includes `native_compositor_auto_sdr_policy_smoke.csv`, which
asserts `macOSPresentationReason=auto-sdr-only`,
`macOSPresentationMode=native-compositor-sdr`, and
`nativeCompositorVideoPixelFormat=32BGRA`.

Portable HLG Auto policy on an EDR-capable display:

```bash
python dev.py gate macos-hdr-edr-smoke
```

This generates a small 10-bit HEVC/HLG fixture and asserts
`macOSPresentationReason=auto-hdr-track`,
`macOSPresentationMode=native-compositor-edr`,
`nativeCompositorVideoPixelFormat=64RGBAHalf`, and
`nativeCompositorEDRVideoMaxRGBX1000 >= 1001`.

SDR content through EDR compositor:

```bash
VOIDPLAYER_MACOS_PRESENTATION_MODE=native-compositor-edr \
  python dev.py mac-ui-test ui_tests/macos/native_compositor_flutter_overlay_smoke.csv
```

This script asserts that SDR remains within reference white while using an EDR
target: `nativeCompositorEDRVideoMaxRGBX1000 == 1000` and
`nativeCompositorEDRVideoPixelsOver1X1000 == 0`.

Local Dolby/HLG sample:

```bash
VOIDPLAYER_MACOS_PRESENTATION_MODE=native-compositor-edr \
  python dev.py mac-ui-test ui_tests/local/dolby_hlg_edr_compositor.csv
```

The local Dolby/HLG CSV asserts that the half-float target contains values
above SDR reference white:

- `nativeCompositorEDRVideoMaxRGBX1000 >= 1001`
- `nativeCompositorEDRVideoPixelsOver1X1000 >= 1`

It references:

```text
/Users/zhuhongwei/Desktop/VID_20260605_212405_DOLBY.mp4
```

That file is not a portable repository fixture.

## Remaining Work

- Add user-facing Auto / Force SDR / Force HDR settings after the default Auto
  policy has soaked.
- Calibrate HLG/PQ mapping against display EDR headroom instead of using the
  current simple HDR constants.
- Add proper Display P3 / Rec.2020 gamut mapping.
- Treat Dolby Vision RPU metadata as later color-quality work; the current
  path uses the base HLG/PQ layer.
- Protect the Flutter fork release ref before any app PR depends on it.
