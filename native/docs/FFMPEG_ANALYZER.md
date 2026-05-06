# FFmpeg Analyzer Tool

VoidPlayer uses the `native/analysis/vendor/ffmpeg` submodule for planned
codec-native VBS4 generation. This analyzer is built out-of-band and installed
as a runtime tool; the main native and Flutter builds do not compile FFmpeg.

## Local Toolchain Shape

Use MSYS2 as the Unix-like configure/make shell, but use the Visual Studio
compiler and linker for the actual Windows binary.

Expected local toolchain shape:

- `C:\msys64\usr\bin\bash.exe` works.
- MSYS packages such as `make`, `pkgconf`, `diffutils`, and `base-devel` are
  present.
- UCRT64 compiler packages are not currently installed.
- Visual Studio 18 has `cl.exe` under
  `C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\...`.
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

The FFmpeg fork carries a VoidPlayer build helper:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File native\analysis\vendor\ffmpeg\voidplayer\build_windows_msvc.ps1
```

Optional overrides:

```powershell
$env:VOID_NASM = "C:\Tools\NASM\nasm.exe"
$env:MSYS2_BASH = "D:\msys64\usr\bin\bash.exe"
```

The script configures a minimal static MSVC build with NASM enabled, builds the
analyzer tool, and installs it to:

```text
native/analysis/vendor/ffmpeg/bin/windows-x64/void_ffmpeg_analyzer.exe
```

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
  --enable-decoder=hevc \
  --enable-parser=hevc \
  --enable-demuxer=mov,matroska,hevc \
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

The runtime tool expected by `windows/runner/analysis_ffi.cpp` is:

```text
native/analysis/vendor/ffmpeg/bin/windows-x64/void_ffmpeg_analyzer.exe
```

or the temporary single-codec name:

```text
native/analysis/vendor/ffmpeg/bin/windows-x64/void_hevc_analyzer.exe
```

The command line contract is:

```text
void_ffmpeg_analyzer.exe --codec hevc --input <video> --vbs4 <output.vbs4>
void_ffmpeg_analyzer.exe --codec h264 --input <video> --vbs4 <output.vbs4>
```

The current tool emits real decoder-derived VBS4 payloads for H.265/H.264. It
still accepts `--probe-only` for quick codec/open validation:

```text
void_ffmpeg_analyzer.exe --codec hevc --input <video> --probe-only
```

Verified VBS4 codecs:

- `hevc` / H.265
- `h264`

AV1, VP9, and MPEG-2 VBS4 generation are disabled until codec-specific payload
profiles are added.

The current PE import table for the analyzer contains only Windows DLLs:

```text
Secur32.dll
ncrypt.dll
CRYPT32.dll
WS2_32.dll
USER32.dll
KERNEL32.dll
```

There are no MSYS2, pthread, libgcc, or libstdc++ runtime imports.
