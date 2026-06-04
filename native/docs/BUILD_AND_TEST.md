# 构建与测试

## 入口命令

项目根目录优先使用 `dev.py`。它会串起 native build、CTest、Flutter build、UI automation 和 package staging。

```bash
python3.12 dev.py build --native
python3.12 dev.py build --flutter
python3.12 dev.py test
python3.12 dev.py test --native-only
python3.12 dev.py gate pr-fast
python3.12 dev.py gate macos-ui-smoke
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

Named gates wrap the canonical command sets:

```bash
python3.12 dev.py gate pr-fast
python3.12 dev.py gate macos-ui-smoke
python3.12 dev.py gate macos-ui-nightly
python3.12 dev.py gate macos-release-readiness
python3.12 dev.py gate windows-preservation
```

Native 子目录仍可直接使用 CMake/presets，但日常开发应通过顶层 `dev.py` 保持平台产物和依赖检查一致。
`native/build.py` 是 `dev.py` 的 standalone CMake helper，不作为常规用户入口记录。

## 构建产物目录

`native/` 是源码目录，新的 native CMake 产物应放在根目录 `build/native/` 下：

| 目录 | 说明 |
| --- | --- |
| `build/native/standalone/` | `dev.py build --native`、native CMake presets、独立 CTest/FFI/Python 产物 |
| `build/native/runner/` | Flutter/Xcode runner 内嵌 native 静态库或中间产物 |
| `build/native/analysis/` | analysis CLI 等辅助 native 工具构建目录 |

旧构建目录如 `native/build-msvc`、`native/build-macos-*` 不属于当前产物布局，不应再新增引用。

## CMake 目标概览

Source ownership is split under `native/cmake/`:

| 文件 | 说明 |
| --- | --- |
| `NativeSourcesPortable.cmake` | shared playback/media/renderer source lists |
| `NativeSourcesWindows.cmake` | Windows facade、D3D11 backend、Windows-only renderer integration |
| `NativeSourcesMacOS.cmake` | macOS native bridge、Metal/CVPixelBuffer presentation |
| `NativeSourcesAnalysis.cmake` | analysis cache/generator source list |
| `NativeSourcesShaders.cmake` | embedded HLSL shader inputs |
| `WindowsBindingTargets.cmake` | Windows FFI / pybind11 binding targets |
| `NativeInstall.cmake` | FFI / Python dist staging rules |

新增 native 源文件时先按 ownership 放进对应清单；`NativeSources.cmake` 只保留 shared path setup、includes 和通用 CMake helper functions。

| 目标 | 平台 | 说明 |
| --- | --- | --- |
| `void_player_portable_core` | all | 平台中立基础类型、layout、presentation contracts |
| `void_media_ffmpeg` | all | FFmpeg demux/decode support |
| `void_renderer_portable_driver` | macOS/native smokes | shared renderer driver object target |
| `void_macos_native_player` | macOS | macOS native bridge + Metal presentation backend |
| `macos_native_abi_smoke` | macOS | macOS C ABI layout/status/lifetime contract smoke |
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

## Test Matrix

测试分层和 ownership 维护在 [TEST_MATRIX.md](TEST_MATRIX.md)。新增或删除测试前，先确认它保护的风险、
所属 gate、是否与已有脚本重复。

## PR Fast Gates

PR fast gate 只放稳定、高信号、非 headed 的检查：

```bash
python3.12 dev.py test --native-only
python3.12 dev.py gate pr-fast
```

Windows CI 还会跑 release compliance notice smoke：

```bash
python3.12 scripts/dev/check_release_compliance.py
```

macOS CI 的 native fast gate uses the hosted-runner CTest profile：

```bash
bash scripts/ci/run_macos_native_fast.sh
```

Hosted macOS PR fast excludes tests labelled `hosted-flaky`; today that is `renderer_metal_headless_smoke`.
Keep that canary for nightly/headed or targeted Metal changes because GitHub runner GPU presentation can report a
black front buffer while the shared native player Metal canary passes.

CTest labels 可用于本地收窄测试：

```bash
ctest --test-dir build/native/standalone/macos-make -L contract --output-on-failure
ctest --test-dir build/native/standalone/macos-make -L backend --output-on-failure
ctest --test-dir build/native/standalone/macos-make -L macos --output-on-failure
ctest --test-dir build/native/standalone/macos-make -LE hosted-flaky --output-on-failure
```

## macOS Stabilization Gates

macOS native playback 当前是 feature-complete / stabilization 状态。修改 shared renderer、Metal backend、VideoToolbox、
Swift texture bridge 或 diagnostics 时，优先使用以下 gate：

```bash
python3.12 dev.py gate macos-ui-smoke
```

更多 macOS UI smokes 按影响面选择：

| 脚本 | 目的 |
| --- | --- |
| `native_facade_smoke.csv` | channel、metadata、diagnostics、首帧健康 |
| `native_controls_smoke.csv` | basic play/pause/seek/step commands |
| `native_seek_frame_smoke.csv` | seek preview / renderer-owned refresh |
| `native_loop_range_smoke.csv` | loop policy |
| `native_audio_play_seek_smoke.csv` | miniaudio/CoreAudio playback、seek、audible-track diagnostics |
| `native_layout_split_smoke.csv` | split/layout and multi-track presentation |
| `native_4k60_playback_smoke.csv` | VideoToolbox CVPixelBuffer + Metal 4K canary |
| `native_vvc_software_playback_smoke.csv` | software fallback + Metal package path |
| `native_p010_presentation_smoke.csv` | 10-bit/P010 presentation path |
| `native_add_short_after_eof_smoke.csv` | EOF carry-forward after adding a shorter third track |
| `native_callback_stress_smoke.csv` | callback lifecycle stress |

Native macOS CTest includes `videotoolbox_provider_smoke`, `macos_metal_uploader_smoke`,
`macos_metal_presentation_backend_smoke`, `renderer_metal_headless_smoke`, and
`macos_native_player_shared_renderer_smoke`.

## Windows Preservation Gate

The macOS backend work changed shared renderer boundaries, so Windows preservation remains a release gate. Run it on a
Windows host before closing macOS release readiness:

```powershell
python dev.py gate windows-preservation
```

Add targeted Windows UI scripts when touching seek, loop, viewport/layout, codec, track, analysis, or D3D11 shared texture/capture behavior.

## Nightly / Headed UI Gates

Headed UI, callback churn, audio speaker behavior, long-run perf, and stress tests should not become default PR fast gates.
Use them for nightly, release candidate, or targeted local validation:

```bash
python3.12 dev.py gate macos-ui-nightly
```

Native sanitizer coverage is a separate local/nightly gate. It builds the macOS native presets with ASAN and TSAN,
then runs CTest while excluding the already-marked headless Metal hosted-flaky smoke:

```bash
python3.12 dev.py gate macos-native-sanitizers
```

`.github/workflows/macos-ui.yml` provides a separate headed macOS UI workflow. It runs `macos-ui-smoke` weekly and can
be manually dispatched with either `macos-ui-smoke` or `macos-ui-nightly`; it is intentionally not part of the regular
push/PR native workflow.

`ui_tests/local/**` is manual/local only and must not be added to CI.

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
python3.12 scripts/dev/check_release_compliance.py --stage build/package/macos/VoidPlayer
python3.12 scripts/dev/check_macos_release_readiness.py --stage build/package/macos/VoidPlayer
```

macOS release-readiness gate wraps package staging and the macOS-specific package smoke:

```bash
python3.12 dev.py gate macos-release-readiness
```

It verifies source and staged-package evidence for FFmpeg dylibs, `@rpath` linkage, GPL/third-party notices,
Release entitlements, sandbox file-picker inputs, crash-report watcher wiring, codesign verification, and absence of
runtime/user artifacts in the package stage. The default gate accepts ad-hoc signing for local testing; use
`scripts/dev/check_macos_release_readiness.py --require-developer-id` on a Developer ID signed stage before external
distribution. Developer ID package/readiness runs also require `spctl -a -vv --type execute` to pass for the staged app.

Release candidate readiness must verify:

- FFmpeg dylibs/runtime files and README/manifest/license notices;
- top-level `LICENSE`, `THIRD_PARTY_NOTICES.md`, and `native/THIRD_PARTY_NATIVE.md`;
- crash/log paths do not land in package staging;
- sandbox file access and file picker behavior;
- signing/notarization inputs for external distribution.
- macOS native ASAN/TSAN gate in `.github/workflows/native.yml`;
- full Windows native config matrix in `.github/workflows/native.yml`;
- Windows UI preservation and macOS headed smoke set selected from [TEST_MATRIX.md](TEST_MATRIX.md).

The local `dev.py gate release-candidate` command does not trigger GitHub's full Windows native config matrix; run the
`Native` workflow manually with `full_matrix=true` or use the scheduled matrix run as separate release evidence.

## CI

Main CI entrypoint is `.github/workflows/native.yml`. It currently covers:

- FFmpeg artifact download once, then per-job restore through `scripts/ci/restore_ffmpeg_*`;
- Windows native fast build/test and release compliance smoke;
- macOS native fast gate through `scripts/ci/run_macos_native_fast.sh`;
- macOS native sanitizer gate on scheduled runs or manual `workflow_dispatch`
  with `macos_sanitizers=true`;
- macOS analysis FFI build smoke plus VAC2/VACHUNK analyzer toolchain smoke;
- macOS runner debug build;
- full Windows native config matrix on scheduled runs or manual `workflow_dispatch` with `full_matrix=true`.

Headed macOS UI evidence lives in `.github/workflows/macos-ui.yml`, which is schedule/manual only.

Do not interpret CI green as full release readiness: headed macOS UI smokes, Windows UI preservation, package staging,
full release compliance against staged packages, and long-run/perf checks remain release-candidate or local gates.

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

These benchmarks are cross-platform for VAC2/VACHUNK generation. On macOS,
`--build` prepares the native analyzer and portable `VoidPlayerCli`; the overlay
GPU timestamp benchmark remains Windows D3D11-only and is skipped by the macOS
dev script. Benchmarks do not replace playback/backend gates.

## Demo

`examples/python/demo_video_renderer.py` and `examples/python/demo_seek.py` are development aids.
Product behavior verification should use `dev.py launch`, native CTest, and platform UI automation.
