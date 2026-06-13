# macOS HDR Native Compositor

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
| Fork release ref | `voidplayer-flutter-3.44.1-hdr.2` |
| Fork patch branch | `voidplayer/hdr-surface-export-3.44.1` |
| Fork commit | `69b3172a210b5c48553db20ae8b7790a45a2036c` |
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
scripts/ci/bootstrap_flutter_macos_engine.sh
```

The fork release also carries the macOS local engine archives required by
`flutter build macos --local-engine`. The asset names and SHA-256 values are
locked in `toolchains/flutter.lock.json` under `macosLocalEngineArtifacts`.
CI restores those archives before macOS runner/UI builds. To publish a new fork
release, build the matching local engine outputs, run:

```bash
scripts/ci/package_flutter_macos_engine.sh debug
scripts/ci/package_flutter_macos_engine.sh release
```

Upload the generated archives to the immutable `VoidPlayer-Flutter` release and
update the lock with the printed hashes.

`voidplayer-flutter-3.44.1-hdr.2` fixes the first native-compositor stability
issue found in this path: Flutter's macOS front-surface list is now exported as
a locked immutable snapshot, so the native compositor can read Flutter's current
texture while Flutter presents the next surface without hitting a mutable-array
enumeration exception.

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

## Mainline Merge Roadmap

This feature should merge as a product path, not as an experimental toggle. The
remaining work is therefore tracked as merge evidence and operational guardrails:

| Stage | Requirement | Status |
| --- | --- | --- |
| Flutter fork pin | VoidPlayer builds must use `toolchains/flutter.lock.json`, the locked `VoidPlayer-Flutter` ref, and matching macOS local-engine archives instead of an ambient developer SDK. | Implemented; keep the fork release tag immutable and update the lock only with a new fork release. |
| Default policy | `auto` must be the default macOS presentation request. SDR media must stay on the SDR compositor, while PQ/HLG media promotes to EDR only when display headroom is available. | Implemented. |
| SDR/HDR composition | Flutter UI, viewport background color, SDR video, and HDR video must be composited in their own color domains; SDR Flutter/background content must not be tone-mapped as HDR. | Implemented for the current native compositor path. |
| Interaction parity | Viewport pan/zoom/split, annotation overlays, timeline controls, resize, seek, EOF, and track removal must behave like the pre-HDR route. | Covered by macOS smoke and targeted local validation; keep adding targeted tests for regressions. |
| Performance | Display-link rendering must avoid hot-path CPU readback and drawable starvation. External CPU pressure should degrade by dropping compositor ticks, not by blocking the UI thread. | Implemented in the compositor refresh path; re-run perf smoke before merge. |
| Color correctness baseline | CPU reference tests and Metal shader tests must agree on range, matrix, transfer, primaries, SDR fallback, and EDR output thresholds. | Implemented as deterministic baseline; subjective tone calibration remains future work. |
| Platform preservation | Windows runner/D3D11 behavior must remain SDR-compatible and must not depend on the Flutter fork patch. | Required before merge; use the Windows preservation gate plus CI/manual evidence. |
| Release readiness | Package/signing checks, FFmpeg dylib staging, notices, toolchain doctor, and macOS release-readiness gate must pass. | Required before merge. |

Merge-blocking validation:

```bash
python3.12 dev.py toolchain doctor
scripts/ci/bootstrap_flutter_macos_engine.sh
python3.12 dev.py gate pr-fast
python3.12 dev.py gate macos-ui-smoke
python3.12 dev.py gate macos-hdr-edr-smoke
python3.12 dev.py gate macos-release-readiness
python dev.py gate windows-preservation
```

`macos-hdr-edr-smoke` requires an EDR-capable local display and covers both
initial HLG Auto promotion and adding an HLG track to an already-created SDR
session. The GitHub-hosted
Windows UI preservation workflow is useful merge evidence, but it runs with
documented CI-only fallbacks because hosted Windows does not expose the same GPU
shape as a real desktop release machine. A local Windows preservation run remains
the release-quality Windows signal.

After these pass, update the PR body with the exact commands, local machine
conditions, and GitHub run URLs, then mark the PR ready for review.

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

These are not merge blockers for the first productized macOS HDR path, but they
should stay visible:

- Add user-facing Auto / Force SDR / Force HDR settings after the default Auto
  policy has soaked.
- Calibrate HLG/PQ mapping against display EDR headroom instead of using the
  current simple HDR constants.
- Treat Dolby Vision RPU metadata as later color-quality work; the current path
  uses the base HLG/PQ layer.
- Rename compatibility aliases and any remaining spike terminology after one
  mainline soak window.
