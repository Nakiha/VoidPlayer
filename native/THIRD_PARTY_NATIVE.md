# Native Third-Party Dependencies

This manifest records the native dependencies that affect reproducible builds,
runtime redistribution, and analysis tooling. Update it in the same change that
updates a pinned dependency, vendored submodule, or runtime package.

| Name | Version / Pin | Source | License | Local Path | Used For | Update Notes |
| --- | --- | --- | --- | --- | --- | --- |
| FFmpeg runtime/dev package | 8.1 full shared Windows build, FFmpeg source commit `9047fa1b08` | gyan.dev package; source commit `https://github.com/FFmpeg/FFmpeg/commit/9047fa1b08` | GPL v3 package, see `windows/libs/ffmpeg/LICENSE*` | `windows/libs/ffmpeg` | demux, decode, D3D11VA, hwdownload, swresample | Replace with same layout: `include/`, `lib/`, `bin/`, `README.txt`, `LICENSE*`; keep runtime DLL copy rules in sync. |
| FFmpeg analyzer fork | submodule commit `0cff290a2c1664f14a05beca661eabb2f8331477` | `native/analysis/vendor/ffmpeg` | FFmpeg upstream licenses plus VoidPlayer analyzer patch licensing | `native/analysis/vendor/ffmpeg` | H.264/HEVC on-demand VACache overlay chunk generation | Update submodule pointer, then rebuild analyzer via `dev.py`; keep stamp logic current. |
| zstd | 1.5.7, submodule commit `f8745da6ff1ad1e7bab384bd1f9d742439278e99` | `native/analysis/vendor/zstd` | BSD/GPL dual license, see `LICENSE` and `COPYING` | `native/analysis/vendor/zstd` | Analyzer chunk compression/decompression compatibility | Update submodule pointer; CMake builds `libzstd_static` with tests/programs disabled. |
| spdlog | commit `48bcf39a661a13be22666ac64db8a7f886f2637e` (`v1.15.2`) | `https://github.com/gabime/spdlog.git` | MIT | FetchContent build tree; optional local cache with `VOID_USE_LOCAL_DEPS=ON` | native logging | Update `native/cmake/Dependencies.cmake` pin and this manifest together. |
| Catch2 | commit `56809e5282f104c5c8b570e7c2996cdc352d94f1` (`v3.8.1`) | `https://github.com/catchorg/Catch2.git` | BSL-1.0 | FetchContent build tree; optional local cache with `VOID_USE_LOCAL_DEPS=ON` | native tests | Update `native/cmake/Dependencies.cmake` pin and this manifest together. |
| pybind11 | resolved by `find_package(pybind11 CONFIG)` / Python package | configured developer or CI Python environment | BSD-style | external environment | Python demo/dev binding | Keep Python binding optional; native build disables it when package discovery fails. |

## Runtime Redistribution

VoidPlayer dynamically links FFmpeg import libraries and redistributes the
required FFmpeg DLLs from `windows/libs/ffmpeg/bin`. The staging rules copy the
runtime DLLs plus `README.txt` and `LICENSE` / `LICENSE.txt` into native build
outputs, FFI/Python dist directories, and Flutter runner outputs.

The default player runtime copies `avcodec`, `avformat`, `avutil`, and
`swresample`. `swscale` is only copied for benchmark targets that explicitly
need it; it is not a general renderer fallback.

## Local Dependency Cache Policy

Network FetchContent dependencies are locked by commit. Existing local source
directories are used only when configuring with `-DVOID_USE_LOCAL_DEPS=ON`.
This keeps clean checkout and CI behavior independent of stale build caches.
