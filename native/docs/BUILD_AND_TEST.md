# 构建与测试

## 入口命令

项目根目录优先使用 `dev.py`。它会串起 native build、CTest、Flutter build、UI automation 和 package staging。

```bash
python3.12 dev.py build --native
python3.12 dev.py build --flutter
python3.12 dev.py test
python3.12 dev.py test --native-only
python3.12 dev.py package
```

Windows UI automation：

```bash
python dev.py ui-test --build ui_tests/smoke/basic.csv
```

macOS UI automation：

```bash
python3.12 dev.py mac-ui-test --build ui_tests/macos/native_facade_smoke.csv
```

Native 子目录仍可直接使用 CMake/presets，但日常开发建议通过顶层 `dev.py` 保持平台产物和依赖检查一致。

## CMake 目标概览

| 目标 | 平台 | 说明 |
| --- | --- | --- |
| `void_player_portable_core` | all | 平台中立基础类型、layout、presentation contracts |
| `void_media_ffmpeg` | all | FFmpeg demux/decode support |
| `void_renderer_portable_driver` | macOS/native smokes | shared renderer driver object target |
| `void_macos_native_player` | macOS | macOS native bridge + Metal presentation backend |
| `video_renderer_ffi` | Windows/native FFI | C FFI shared library，导出 `naki_vr_*` |
| `video_renderer_native` | Windows/Python | pybind11 module for demo/scripts |
| `VoidPlayerCli` | analysis enabled | analysis cache/overlay CLI |
| `video_renderer_tests` | Windows | Catch2 renderer/unit/integration tests |
| `analysis_tests` | analysis enabled | VAC2/VACHUNK/cache tests |
| `test_ffi_c` | FFI tests | C ABI smoke |
| `macos_*_smoke` / `renderer_metal_headless_smoke` | macOS | Metal、VideoToolbox、native player、capture、audio native smokes |

常用 CMake 开关：

| 开关 | 默认 | 说明 |
| --- | --- | --- |
| `BUILD_FFI` | `ON` | 构建 C FFI target |
| `BUILD_PYTHON` | `ON` | 构建 pybind11 Python binding；找不到依赖时自动关闭 |
| `BUILD_TESTS` | `ON` | 构建 CTest targets |
| `BUILD_ANALYSIS` | `ON` | 构建 analysis cache/overlay/CLI；关闭时 renderer 使用 no-op overlay stub |
| `BUILD_BENCHMARKS` | `OFF` | 构建 benchmark targets |

## macOS Stabilization Gates

macOS native playback 当前是 feature-complete / stabilization 状态。修改 shared renderer、Metal backend、VideoToolbox、
Swift texture bridge 或 diagnostics 时，优先使用以下 gate：

```bash
python3.12 dev.py test --native-only
python3.12 dev.py build --flutter
python3.12 dev.py mac-ui-test --build \
  ui_tests/macos/native_facade_smoke.csv \
  ui_tests/macos/native_4k60_playback_smoke.csv \
  ui_tests/macos/native_vvc_software_playback_smoke.csv \
  ui_tests/macos/native_add_track_smoke.csv
```

更多 macOS UI smokes 按影响面选择：

| 脚本 | 目的 |
| --- | --- |
| `native_playback_smoke.csv` | basic play/pause playback |
| `native_seek_frame_smoke.csv` | seek preview / renderer-owned refresh |
| `native_loop_range_smoke.csv` | loop policy |
| `native_audio_play_seek_smoke.csv` | miniaudio/CoreAudio playback + seek |
| `native_layout_split_smoke.csv` | split/layout and multi-track presentation |
| `native_4k60_playback_smoke.csv` | VideoToolbox CVPixelBuffer + Metal 4K canary |
| `native_vvc_software_playback_smoke.csv` | software fallback + Metal package path |
| `native_p010_presentation_smoke.csv` | 10-bit/P010 presentation path |
| `native_callback_stress_smoke.csv` | callback lifecycle stress |

Native macOS CTest includes `videotoolbox_provider_smoke`, `macos_metal_uploader_smoke`,
`macos_metal_presentation_backend_smoke`, `renderer_metal_headless_smoke`, and
`macos_native_player_shared_renderer_smoke`.

## Windows Preservation Gate

The macOS backend work changed shared renderer boundaries, so Windows preservation remains a release gate. Run it on a
Windows host before closing macOS release readiness:

```powershell
python dev.py test --native-only
flutter build windows --release
python dev.py ui-test --build ui_tests/smoke/basic.csv
```

Add targeted Windows UI scripts when touching seek, loop, viewport/layout, codec, track, analysis, or D3D11 shared texture/capture behavior.

## Release / Package Checks

`python3.12 dev.py package` builds/stages clean package input for the current platform. On macOS it stages
`VoidPlayer.app`, copies release docs and FFmpeg compliance files, verifies linkage/codesign, and can optionally create a DMG.

```bash
python3.12 dev.py package
python3.12 dev.py package --installer
python3.12 dev.py package --installer --macos-notarize --macos-notary-profile PROFILE
```

Release compliance smoke:

```bash
python3.12 scripts/dev/check_release_compliance.py
python3.12 scripts/dev/check_release_compliance.py --stage build/package/macos/stage
```

Release readiness must verify:

- FFmpeg dylibs/runtime files and README/manifest/license notices;
- top-level `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `native/THIRD_PARTY_NATIVE.md`;
- crash/log paths do not land in package staging;
- sandbox file access and file picker behavior;
- signing/notarization inputs for external distribution.

## CI

CI entrypoint is `.github/workflows/native.yml`. It currently covers Windows native tests, macOS native tests, macOS
analysis build, macOS runner build, FFmpeg package restore, release compliance smoke, and a VideoToolbox native smoke.

Do not interpret CI green as full release readiness: headed macOS UI smokes, Windows UI preservation, package staging, and
long-run/perf checks remain local or future nightly gates.

## Analysis Benchmarks

Analysis full-file benchmark:

```bash
python dev.py analysis-benchmark --build
python dev.py analysis-benchmark h264 h265 h266
```

Analysis overlay benchmark:

```bash
python dev.py analysis-overlay-benchmark --build
python dev.py analysis-overlay-benchmark --iterations 240 --with-grid
```

These benchmarks are Windows-oriented today and do not replace playback/backend gates.

## Demo

`video_renderer/demo/demo_video_renderer.py` and `video_renderer/demo/demo_seek.py` are development aids. Product behavior
verification should use `dev.py launch`, native CTest, and platform UI automation.
