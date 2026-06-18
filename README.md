# VoidPlayer

VoidPlayer is a Flutter desktop video player with a shared native C++ playback
and rendering core. Flutter/Dart owns the desktop UI shell, actions, automation
entry points, and platform service wiring; native code owns FFmpeg demux/decode,
playback timing, seek/loop behavior, layout, and presentation scheduling.

## Repository Map

| Path | Role |
| --- | --- |
| `lib/` | Flutter UI, main-window orchestration, actions, storage, automation, and platform bindings. |
| `native/` | Shared C++ renderer, media pipeline, analysis tooling, platform backends, and native tests. |
| `windows/` | Flutter Windows runner, D3D11/DComp integration, Windows services, and packaging inputs. |
| `macos/` | Flutter macOS runner, Cocoa/Swift bridge, Metal/CVPixelBuffer presentation, and packaging inputs. |
| `scripts/` | Development, CI, packaging, toolchain, and verification helpers. |
| `toolchains/` | Lock files for build-critical external toolchains. Hydrated toolchains live under `.toolchains/`. |
| `ui_tests/` | CSV UI automation scripts and gate profile definitions. |
| `resources/` | Checked-in media fixtures used by tests. |

## Common Commands

```bash
python dev.py build --native
python dev.py test
python dev.py gate pr-fast
python dev.py ui-test --build ui_tests/smoke/basic.csv
python dev.py mac-ui-test --build ui_tests/macos/native_facade_smoke.csv
```

FFmpeg runtime/dev SDKs are pinned by `toolchains/ffmpeg.lock.json` and restored
to `.toolchains/ffmpeg/`; they are not committed. Flutter fork inputs are pinned
by `toolchains/flutter.lock.json`.

## Documentation

- Flutter/Dart layer: [lib/doc.md](lib/doc.md)
- Windows runner: [windows/doc.md](windows/doc.md)
- macOS runner: [macos/doc.md](macos/doc.md)
- Native architecture: [native/docs/ARCHITECTURE.md](native/docs/ARCHITECTURE.md)
- Native build and tests: [native/docs/BUILD_AND_TEST.md](native/docs/BUILD_AND_TEST.md)
- Native target boundaries: [native/docs/TARGET_BOUNDARIES.md](native/docs/TARGET_BOUNDARIES.md)
- macOS readiness: [native/docs/MACOS_READINESS.md](native/docs/MACOS_READINESS.md)
