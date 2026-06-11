# VoidPlayer Flutter Fork Patch Inventory

VoidPlayer pins a Flutter fork because the macOS HDR/native-compositor path
needs access to Flutter's current rendered surface. The app does not rely on a
developer's ambient Flutter SDK; `toolchains/flutter.lock.json` is the source of
truth.

## Pinned Ref

| Item | Value |
| --- | --- |
| Fork repo | `https://github.com/Nakiha/VoidPlayer-Flutter.git` |
| Release ref | `voidplayer-flutter-3.44.1-hdr.1` |
| Patch branch | `voidplayer/hdr-surface-export-3.44.1` |
| Baseline branch | `voidplayer/flutter-3.44.1-baseline` |
| Baseline tag | `3.44.1` |
| Fork commit | `04b75e628e3a7c7ffc66f14e50f760564ab2e9f2` |
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

Changed Flutter files in `04b75e628e3`:

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
3. Build the required local engine artifacts.
4. Create a new immutable release tag, for example
   `voidplayer-flutter-3.44.1-hdr.2`.
5. Update `toolchains/flutter.lock.json`.
6. Run:

```bash
python dev.py toolchain bootstrap-flutter
python dev.py toolchain doctor
python dev.py test --flutter-only
python dev.py build --flutter --debug
```

7. Re-run the macOS HDR compositor smoke scripts documented in
   `native/docs/MACOS_HDR_EXPLORATION.md`.

Do not move an existing `voidplayer-flutter-*-hdr.*` tag after VoidPlayer has
pointed at it. Publish a new tag instead.
