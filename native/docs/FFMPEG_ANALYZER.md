# FFmpeg Analyzer Tool

VoidPlayer uses the `native/analysis/vendor/ffmpeg` submodule for codec-native
VACHUNK overlay generation. This analyzer is built out-of-band and installed
as a runtime tool; the main native and Flutter builds do not compile FFmpeg.

## Local Toolchain Shape

Use MSYS2 as the Unix-like configure/make shell, but use the Visual Studio
compiler and linker for the actual Windows binary.

Expected local toolchain shape:

- `C:\msys64\usr\bin\bash.exe` works.
- MSYS packages such as `make`, `pkgconf`, `diffutils`, and `base-devel` are
  present.
- UCRT64 compiler packages are not currently installed.
- Visual Studio 2022 or newer has `cl.exe` under
  `C:\Program Files\Microsoft Visual Studio\...\VC\Tools\MSVC\...`.
- NASM is installed and either available as `nasm.exe` on `PATH`, or provided
  through `VOID_NASM` / `VOID_NASM_DIR`.

That is enough for the first MSVC-based analyzer build. The UCRT64 GCC/Clang
packages are not required if FFmpeg is configured with `--toolchain=msvc`.
The build helper resolves NASM in this order:

1. `-NasmPath` / `VOID_NASM`, pointing directly to `nasm.exe`
2. `-NasmDir` / `VOID_NASM_DIR`, pointing to the directory containing `nasm.exe`
3. `nasm.exe` on `PATH`
4. common system install locations such as `C:\Program Files\NASM`

## Runtime Dependency Rule

The analyzer executable must be a native Windows PE and must not import:

- `msys-2.0.dll`
- `libpthread*.dll`
- `libgcc_s*.dll`
- `libstdc++-6.dll`

MSYS2 is allowed at build time only.

## Build Command

The dev script owns the analyzer build orchestration:

```bash
python dev.py build --native
```

Optional overrides:

```powershell
$env:VOID_NASM = "C:\Tools\NASM\nasm.exe"
$env:MSYS2_BASH = "D:\msys64\usr\bin\bash.exe"
$env:CMAKE_GENERATOR = "Visual Studio 17 2022"
```

The Python build helper configures a minimal static build, builds the analyzer
tool, and installs it to the host platform bin directory:

```text
native/analysis/vendor/ffmpeg/bin/windows-x64/void_ffmpeg_analyzer.exe
native/analysis/vendor/ffmpeg/bin/macos-arm64/void_ffmpeg_analyzer
native/analysis/vendor/ffmpeg/bin/macos-x64/void_ffmpeg_analyzer
```

`python dev.py build --native` / `python dev.py ui-test --build ...` install the
Windows tool into the Flutter runner output under:

```text
build/windows/x64/runner/<Config>/tools/ffmpeg-analysis/void_ffmpeg_analyzer.exe
```

On macOS the analyzer is built by `python dev.py analysis-benchmark --build`,
`python dev.py analysis-overlay-benchmark --build`, `python dev.py build --flutter`,
`python dev.py launch`, `python dev.py mac-ui-test --build ...`, or by the macOS analysis
CI smoke before CMake configures `macos_analysis_toolchain_smoke`. The macOS build uses
clang/darwin FFmpeg static archives and the same zstd-backed VACHUNK writer. Dev Flutter
builds install the tool into the app bundle under:

```text
build/macos/Build/Products/<Config>/VoidPlayer.app/Contents/Helpers/ffmpeg-analysis/void_ffmpeg_analyzer
```

The dev script stamps the analyzer with a signature over the Python build helper and
codec hook files/headers. When H.264/HEVC/VVC hook code changes, the next build
forces a clean analyzer rebuild so stale object files do not leave an old tool
beside a freshly built runner.

The current configure shape is:

```bash
./configure \
  --toolchain=msvc \
  --arch=x86_64 \
  --target-os=win64 \
  --disable-shared \
  --enable-static \
  --disable-programs \
  --disable-doc \
  --disable-everything \
  --enable-decoder=h264,hevc,vvc \
  --enable-parser=h264,hevc,vvc \
  --enable-demuxer=mov,matroska,h264,hevc,vvc \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,vvc_mp4toannexb \
  --enable-protocol=file
```

If a different machine cannot provide NASM, temporarily add `--disable-x86asm`
to prove the analyzer plumbing first. That should be treated as a local build
workaround, not the preferred release configuration.

The local FFmpeg `configure` script has a VoidPlayer patch that recognizes
localized MSVC output where `Microsoft` is not the first token on the `cl.exe`
banner line. Without it, FFmpeg misclassifies the compiler and passes GCC-style
`-o` arguments to `cl.exe`.

## Runtime Contract

The runtime tool expected by Windows analysis FFI is:

```text
native/analysis/vendor/ffmpeg/bin/windows-x64/void_ffmpeg_analyzer.exe
```

The command line contract is:

```text
void_ffmpeg_analyzer --codec vvc --input <video> --vachunk <output.vck> --start-frame <n> --end-frame <m>
void_ffmpeg_analyzer --codec hevc --input <video> --vachunk <output.vck> --start-frame <n> --end-frame <m>
void_ffmpeg_analyzer --codec h264 --input <video> --vachunk <output.vck> --start-frame <n> --end-frame <m>
```

The current tool emits real decoder-derived overlay VACHUNK payloads for
H.266/H.265/H.264. Overlay records include CU/MB geometry, QP, prediction mode,
motion vectors/reference indexes where available, and per-CU/MB coded bit
counts used by the bit-cost heatmap. The tool writes a temporary uncompressed
VCK1 file; the runner/CLI publish step validates that file and rewrites it
through the native VACHUNK writer, which may zstd-compress individual payload
sections before atomically publishing into cache. It still accepts `--probe-only`
for quick codec/open validation:

```text
void_ffmpeg_analyzer --codec hevc --input <video> --probe-only
```

Verified overlay VACHUNK codecs:

- `vvc` / H.266
- `hevc` / H.265
- `h264`

AV1, VP9, and MPEG-2 overlay generation are disabled until codec-specific
payload profiles are added.

The Windows PE import table for the analyzer contains only Windows DLLs:

```text
Secur32.dll
ncrypt.dll
CRYPT32.dll
WS2_32.dll
USER32.dll
KERNEL32.dll
```

There are no MSYS2, pthread, libgcc, or libstdc++ runtime imports.

## Benchmark Flow

Use the dev benchmark to measure full-file VAC2 + overlay VACHUNK generation for
the supported codec samples:

```powershell
python dev.py analysis-benchmark --build
python dev.py analysis-benchmark h264 h265 h266
```

The command runs `VoidPlayerCli generate-base` and `generate-overlay` over the
selected samples, then inspects the published chunks. On macOS, `--build`
builds the portable CLI and analyzer from native CMake/FFmpeg rather than a
Flutter runner. Reports are written to
`build/analysis-benchmark/analysis_benchmark.json` and `.md`, including elapsed
time, cache/video size ratio, section decoded/compressed sizes, and zstd savings.

The analyzer already disables loop filtering and IDCT where FFmpeg permits it,
but still must demux, send packets, and receive decoded frames because the
codec-specific CU hooks fire inside decoder paths. Further pixel-path removal
should be checked by comparing generated VAC2/VACHUNK records before and after
the change, not just by wall-clock time.
