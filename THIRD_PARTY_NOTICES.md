# Third-Party Notices

VoidPlayer is distributed under the GNU General Public License v3.0; see
`LICENSE`.

This file is the top-level release notice for dependencies that are bundled,
staged, or materially used by the Windows native player. Native-specific
version pins and update notes live in `native/THIRD_PARTY_NATIVE.md`.

## FFmpeg Runtime Package

The default Windows FFmpeg runtime/dev package is the gyan.dev FFmpeg 8.1 full
shared build stored under `windows/libs/ffmpeg`. That package is GPL v3 and is
dynamically linked by VoidPlayer/native through FFmpeg import libraries.

Release artifacts that include FFmpeg DLLs must include the FFmpeg package
`README.txt` and `LICENSE` / `LICENSE.txt` files. The FFmpeg README records the
source commit and configure flags for the bundled package. Users may replace
the FFmpeg runtime with a compatible package that provides the same `include/`,
`lib/`, `bin/`, `README.txt`, and `LICENSE*` layout.

## Native Dependencies

The native module also uses:

- zstd, from `native/analysis/vendor/zstd`, for VBS4 compression.
- spdlog, fetched by pinned commit unless `VOID_USE_LOCAL_DEPS=ON`.
- Catch2, fetched by pinned commit for tests.
- VTM / analyzer tooling under `native/analysis/vendor/`.

See `native/THIRD_PARTY_NATIVE.md` for versions, source locations, licenses,
and update notes.

## Flutter / App Dependencies

Flutter, Dart packages, and vendored Flutter plugins are tracked by
`pubspec.yaml`, `pubspec.lock`, and their local license files. The vendored
`third_party/desktop_drop` plugin includes its own `LICENSE`.
