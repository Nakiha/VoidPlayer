# VoidPlayer Flutter Fork Patch Inventory

VoidPlayer pins a Flutter fork because the macOS HDR/native-compositor path
needs access to Flutter's current rendered surface. The app does not rely on a
developer's ambient Flutter SDK; `toolchains/flutter.lock.json` is the source of
truth.

## Pinned Ref

| Item | Value |
| --- | --- |
| Fork repo | `https://github.com/Nakiha/VoidPlayer-Flutter.git` |
| Fork ref | `codex/windows-surface-export-pacing` |
| Patch branch | `codex/windows-surface-export-pacing` |
| macOS local engine release tag | `voidplayer-flutter-3.44.1-hdr.2` |
| Windows local engine release tag | `voidplayer-flutter-dd94358590cf-windows.1` |
| Baseline branch | `voidplayer/flutter-3.44.1-baseline` |
| Baseline tag | `3.44.1` |
| Fork commit | `dd94358590cf3de70bff7c60bfb7f41f33146b3b` |
| Framework revision | `dd94358590cf3de70bff7c60bfb7f41f33146b3b` |
| Engine revision | `c416acfeb8126e097f758c664aaa3da929e27da0` |
| Dart SDK | `3.12.1` |

`flutter --version --machine` may report different human-readable
`flutterVersion` values for official-checkout and fork-checkout shapes. Treat
that field as informational. VoidPlayer's hard lock is the framework revision,
engine revision, Dart SDK version, clean git checkout, and patch markers.

## Patch Summary

The current fork exposes the macOS Flutter backing surface to the runner for
the HDR compositor. The app uses this to composite native video under the
Flutter UI in a native `CAMetalLayer`.

Changed Flutter files through `dd94358590cf`:

| File | Purpose |
| --- | --- |
| `engine/src/flutter/shell/platform/darwin/macos/framework/Headers/FlutterEngine.h` | Adds the private VoidPlayer surface-info accessor. |
| `engine/src/flutter/shell/platform/darwin/macos/framework/Source/FlutterEngine.mm` | Implements surface-info serialization and debug channel plumbing. |
| `engine/src/flutter/shell/platform/darwin/macos/framework/Source/FlutterSurface.h` | Exposes IOSurface/Metal texture metadata internally. |
| `engine/src/flutter/shell/platform/darwin/macos/framework/Source/FlutterSurface.mm` | Carries the backing IOSurface/texture through the surface wrapper. |
| `engine/src/flutter/shell/platform/darwin/macos/framework/Source/FlutterSurfaceManager.mm` | Keeps presented front surfaces discoverable. |
| `engine/src/flutter/shell/platform/darwin/macos/framework/Source/FlutterSurfaceManagerTest.mm` | Covers the surface manager behavior changed by the compositor export patch. |
| `engine/src/flutter/shell/platform/darwin/macos/framework/Source/FlutterCompositor.mm` | Adds compositor diagnostics around surface flow. |
| `engine/src/flutter/shell/platform/darwin/macos/framework/Source/FlutterDartProject.mm` | Adds VoidPlayer compositor configuration plumbing. |
| `engine/src/flutter/shell/platform/embedder/embedder.cc` | Adds temporary HDR compositor diagnostics for Metal backing-store wrapping. |
| `engine/src/flutter/shell/platform/embedder/embedder_external_view_embedder.cc` | Adds temporary HDR compositor diagnostics for external-view submit/target flow. |
| `engine/src/flutter/shell/platform/windows/public/flutter_windows.h` | Adds the Windows surface-export C ABI, including compositor-owned frame request and state diagnostics. |
| `engine/src/flutter/shell/platform/windows/flutter_windows.cc` | Exposes Windows surface-export mode, request, state, acquire, and release entrypoints. |
| `engine/src/flutter/shell/platform/windows/flutter_windows_view.h` / `.cc` | Keeps compositor-owned export frames schedulable without switching to mirror or HWND present. |
| `engine/src/flutter/shell/platform/windows/flutter_windows_surface_export.h` / `.cc` | Implements the Windows surface export V2 backend negotiation path and request/publish/backpressure diagnostics. |
| `engine/src/flutter/shell/platform/windows/compositor_opengl.cc` | Restores the bounded compositor-owned frame pump after successful surface exports so DComp-visible Flutter UI keeps publishing during native compositor interaction bursts. |
| `engine/src/flutter/shell/platform/windows/compositor_opengl_unittests.cc` / `flutter_windows_view_unittests.cc` | Covers export generation/state and compositor-owned request scheduling. |

The current lock also carries the Windows surface-export patch line. Windows
runner builds use this to access the Flutter surface through the native
compositor contract instead of falling back to a Flutter Texture path. Windows
surface-export V2 leases are D3D12-only: D3D11 V2 acquisition fails closed so
VoidPlayer cannot silently return to the legacy DX11 consumer path.

`voidplayer-flutter-3.44.1-hdr.2` stabilizes the exported macOS front surface
list: `FlutterSurfaceManager.frontSurfaces` returns an immutable snapshot while
mutations are locked, and `FlutterEngine` enumerates only that snapshot. This
prevents the native compositor from crashing with "collection was mutated while
being enumerated" when Flutter presents a new surface while VoidPlayer reads
the current Flutter texture.

## Local Workflow

```bash
python dev.py toolchain bootstrap-flutter
python dev.py toolchain doctor
```

Developers may point `dev_config.local.json` at an existing Flutter fork
checkout, but every `dev.py` Flutter build/test path still verifies that the
checkout matches `toolchains/flutter.lock.json`.

## Upgrade Procedure

1. Create a new baseline branch/tag in `Nakiha/VoidPlayer-Flutter`.
2. Rebase or replay the VoidPlayer patch branch on that baseline.
3. Build the required local engine artifacts:

```bash
scripts/ci/package_flutter_macos_engine.sh debug
scripts/ci/package_flutter_macos_engine.sh release
scripts/ci/package_flutter_windows_engine.ps1 -Mode debug
scripts/ci/package_flutter_windows_engine.ps1 -Mode release
```

4. Create a new immutable release tag, for example
   `voidplayer-flutter-3.44.1-hdr.2` for macOS or
   `voidplayer-flutter-dd94358590cf-windows.1` for Windows.
5. Upload the generated `*-macos-host_*.tar.gz` or
   `*-windows-host_*.zip` files to that release.
6. Update `toolchains/flutter.lock.json`, including asset names and SHA-256
   values under `macosLocalEngineArtifacts` or
   `windowsLocalEngineArtifacts`. If the Windows engine artifacts change,
   update `windowsEngineReleaseTag` in the same lock change.
7. Keep this patch inventory in sync with the lock:
   `forkRef`, `forkBranch`, `forkCommit`, `frameworkRevision`,
   `engineRevision`, `dartSdkVersion`, `macosLocalEngineReleaseTag`, and
   `windowsEngineReleaseTag` must all match `toolchains/flutter.lock.json`.
8. Run:

```bash
python dev.py gate flutter-fork-protection
python dev.py toolchain bootstrap-flutter
python dev.py toolchain doctor
python dev.py test --flutter-only
python dev.py build --flutter --debug
```

For normal development, `python dev.py toolchain bootstrap-flutter` wraps the
Flutter checkout bootstrap and locked local-engine downloads. The standalone
macOS and Windows bootstrap scripts remain available for CI and artifact
debugging.

9. Re-run the macOS HDR compositor smoke scripts documented in
   `native/docs/MACOS_HDR_EXPLORATION.md`.
10. For Windows surface-export changes, run `python dev.py gate
    windows-preservation` with the locked Windows local engine. Hosted CI can
    validate PR-fast native contracts, but ordinary Flutter SDK fallback is not
    release evidence for compositor-surface changes.

Do not move an existing `voidplayer-flutter-*-hdr.*` tag after VoidPlayer has
pointed at it. Publish a new tag instead.
