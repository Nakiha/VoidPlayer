# Native Target Boundaries

本文档记录 native CMake target 的 public/internal 边界。它不是一次性重命名计划，而是后续拆分时的约束：先保证 feature option 可验证，再逐步把 target 名称收敛到职责边界。

## Current Targets

| Target | Boundary | Public Surface |
| --- | --- | --- |
| `void_player_portable_core` | macOS-buildable playback clock, logging, packet queue, seek coordinator, frame buffers, render sink | Internal static library; first Phase 2 portability target |
| `void_media_ffmpeg` | macOS-buildable FFmpeg demux/private CDN FLV demux layer plus decode/exact-seek policies that do not own texture conversion | Internal static library depending on `void_player_portable_core` |
| `macos_media_smoke` | CLI/CTest probe that opens a bundled media fixture through `void_media_ffmpeg` | macOS-only validation executable |
| `video_renderer_core` | FFmpeg demux/decode common logic, playback clock, queues, buffers, sync policies | Internal static library |
| `video_renderer_lib` | Windows player/renderer facade, audio, D3D11 backend, optional analysis overlay implementation | Internal static library consumed by FFI/Python/tests |
| `analysis_lib` | VAC2/VACHUNK cache, parsers, generators, analysis sessions | Internal static library, only when `BUILD_ANALYSIS=ON` |
| `video_renderer_ffi` | C ABI exported through `video_renderer/exports/ffi_exports.h` | Public native ABI |
| `video_renderer_native` | pybind11 module for local demo/tooling | Developer-facing module |
| `VoidPlayerCli` | Analysis cache inspection/generation CLI | Release/tooling executable, only when `BUILD_ANALYSIS=ON` |
| `video_renderer_tests` / `analysis_tests` / `test_ffi_c` | CTest coverage | Test-only |

## Feature Options

| Option | Default | Required Behavior |
| --- | --- | --- |
| `BUILD_ANALYSIS` | `ON` | `OFF` must build renderer/player/FFI without `analysis_lib`, zstd, analysis tests, or `VoidPlayerCli`; renderer overlay draw is a no-op stub. |
| `BUILD_FFI` | `ON` | `OFF` must not create `video_renderer_ffi`, FFI dist staging, or `test_ffi_c`. |
| `BUILD_PYTHON` | `ON` | `OFF` must not create `video_renderer_native` or Python dist staging. |
| `BUILD_TESTS` | `ON` | `OFF` must not create CTest targets. |
| `BUILD_BENCHMARKS` | `OFF` | `ON` may build benchmark executables; benchmarks are not public runtime dependencies. |

## Planned Split

The next target names should be introduced only when they remove a real dependency edge:

| Planned Target | Owns | Must Not Own |
| --- | --- | --- |
| `void_core` | logging helpers, limits, pure policies, clocks, thread-safe queues | FFmpeg headers, D3D11, Flutter, analysis cache |
| `void_render_d3d11` | D3D11 device, shaders, frame presenter, headless output, capture | analysis parsers/generators, public ABI wrappers |
| `void_player` | NativePlayer facade, playback/audio/renderer orchestration | Flutter MethodChannel, C ABI exports |
| `void_analysis` | VAC2/VACHUNK parser/generator/session/cache targets | renderer/player ownership |
| `void_ffi` | `naki_vr_*` C ABI and flat error/status contract | Flutter runner process globals |
| `void_flutter_windows_plugin` | Flutter runner bridge and MethodChannel/EventChannel glue | reusable native rendering logic |

## Policy

- Public ABI is limited to exported headers and symbols, currently `video_renderer/exports/ffi_exports.h` and `naki_vr_*`.
- Static libraries are internal unless explicitly documented here as release/tooling surfaces.
- A target may depend on a more foundational target, but foundational targets must not include higher-level feature headers just for convenience.
- New feature options need one default-path verification and one disabled-path verification before the tracker can mark them done.
- Optional features should fail closed: disabled targets should not leave empty dist folders, dangling tests, or generated projects that link missing libraries.
