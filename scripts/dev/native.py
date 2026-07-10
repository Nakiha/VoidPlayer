"""Native standalone build and test commands."""

import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

from .paths import (
    FFMPEG_ANALYZER_DIR,
    MACOS_FFMPEG_ROOT,
    MACOS_NATIVE_ANALYSIS_BUILD_DIR,
    MACOS_NATIVE_MAKE_BUILD_DIR,
    NATIVE_BUILD_PY,
    NATIVE_DIR,
    ROOT,
    executable_name,
    find_ffmpeg_analyzer,
    host_platform_id,
)
from .process import header, run

TOOL_STAMP_VERSION = 2
FFMPEG_SUBMODULE_PATH = "native/analysis/vendor/ffmpeg"
ZSTD_SUBMODULE_PATH = "native/analysis/vendor/zstd"


def _env_flag(name: str) -> bool:
    return os.environ.get(name, "").lower() in {"1", "true", "yes", "on"}


def ensure_analysis_test_tools() -> None:
    """Prepare external tools required by native analysis tests."""
    ensure_ffmpeg_analyzer_tool()


def build_macos_analysis_cli() -> None:
    """Build the portable macOS analysis CLI used by analysis benchmarks."""
    if sys.platform != "darwin":
        raise RuntimeError("macOS analysis CLI build is only supported on macOS.")

    ensure_ffmpeg_analyzer_tool()
    build_dir = MACOS_NATIVE_ANALYSIS_BUILD_DIR
    header("Build macOS analysis CLI")
    run([
        "cmake",
        "-S",
        str(NATIVE_DIR),
        "-B",
        str(build_dir),
        "-G",
        "Unix Makefiles",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_ANALYSIS=ON",
        "-DBUILD_ANALYSIS_TESTS=OFF",
        "-DBUILD_TESTS=ON",
        "-DBUILD_PYTHON=OFF",
        f"-DFFMPEG_ROOT={MACOS_FFMPEG_ROOT}",
        *(
            ["-DVOID_USE_LOCAL_DEPS=ON"]
            if _env_flag("VOID_USE_LOCAL_DEPS")
            else []
        ),
    ], cwd=str(ROOT))
    run([
        "cmake",
        "--build",
        str(build_dir),
        "--target",
        "VoidPlayerCli",
        "--",
        f"-j{os.cpu_count() or 4}",
    ], cwd=str(ROOT))


def _file_sha256(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def _source_hashes(root: Path, patterns: tuple[str, ...]) -> dict[str, str]:
    hashes: dict[str, str] = {}
    for pattern in patterns:
        matches = sorted(
            path for path in root.glob(pattern)
            if path.is_file() and "build" not in path.relative_to(root).parts
        )
        if not matches:
            hashes[pattern] = "missing"
            continue
        for path in matches:
            relative = path.relative_to(root).as_posix()
            hashes[relative] = _file_sha256(path)
    return hashes


def _git_head(path: Path) -> str:
    if not path.exists():
        return "missing"

    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=str(path),
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode == 0:
        return result.stdout.strip()
    return "unknown"


def _read_json(path: Path) -> dict | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _tool_needs_rebuild(exe: Path, stamp: Path, signature: dict, label: str) -> bool:
    if not exe.exists():
        print(f"{label} missing; building it...")
        return True

    current = _read_json(stamp)
    if current != signature:
        if current is None:
            print(f"{label} has no build stamp; rebuilding it...")
        else:
            print(f"{label} build stamp is stale; rebuilding it...")
        return True

    return False


def _zstd_head() -> str:
    return _git_head(FFMPEG_ANALYZER_DIR.parent / "zstd")


def _ffmpeg_analyzer_signature() -> dict:
    source_hashes = _source_hashes(FFMPEG_ANALYZER_DIR, (
        "tools/void_ffmpeg_analyzer.c",
        "libavcodec/h264dec.h",
        "libavcodec/h264_slice.c",
        "libavcodec/h264_mb.c",
        "libavcodec/h264_cabac.c",
        "libavcodec/h264_cavlc.c",
        "libavcodec/cabac.c",
        "libavcodec/cabac.h",
        "libavcodec/cabac_functions.h",
        "libavcodec/hevc/hevcdec.c",
        "libavcodec/hevc/*.h",
        "libavcodec/vvc/ctu.c",
        "libavcodec/vvc/*.h",
        "libavcodec/voidplayer_vachunk.c",
        "libavcodec/voidplayer_vachunk.h",
    ))

    return {
        "version": TOOL_STAMP_VERSION,
        "tool": "ffmpeg-analyzer",
        "configuration": f"analysis-minimal-{host_platform_id()}",
        "ffmpeg_head": _git_head(FFMPEG_ANALYZER_DIR),
        "zstd_head": _zstd_head(),
        "python_builder_sha256": _file_sha256(Path(__file__)),
        "source_sha256": source_hashes,
    }


def _ffmpeg_analyzer_stamp_path() -> Path:
    return (
        FFMPEG_ANALYZER_DIR
        / "build"
        / f"voidplayer-analyzer-{host_platform_id()}-stamp.json"
    )


def _write_ffmpeg_analyzer_stamp(analyzer: Path) -> None:
    _write_json(_ffmpeg_analyzer_stamp_path(), _ffmpeg_analyzer_signature())


def _ffmpeg_analyzer_source_ready() -> bool:
    zstd_cmake = FFMPEG_ANALYZER_DIR.parent / "zstd" / "build" / "cmake" / "CMakeLists.txt"
    return (
        (FFMPEG_ANALYZER_DIR / "configure").is_file()
        and (FFMPEG_ANALYZER_DIR / "tools" / "void_ffmpeg_analyzer.c").is_file()
        and zstd_cmake.is_file()
    )


def _ensure_ffmpeg_analyzer_submodule() -> None:
    """Ensure the patched FFmpeg analyzer source checkout is usable."""
    if _ffmpeg_analyzer_source_ready():
        return

    print("FFmpeg analyzer source tree missing or incomplete. Running git submodule update...")
    try:
        run([
            "git", "submodule", "update", "--init", "--recursive", "--checkout",
            FFMPEG_SUBMODULE_PATH, ZSTD_SUBMODULE_PATH,
        ], cwd=str(ROOT))
    except subprocess.CalledProcessError:
        print(
            "\nERROR: failed to initialize FFmpeg analyzer submodules.\n"
            "Try one of:\n"
            f"  git submodule update --init --recursive --checkout {FFMPEG_SUBMODULE_PATH} {ZSTD_SUBMODULE_PATH}\n"
            f"  VOID_FFMPEG_ANALYZER=/path/to/{executable_name('void_ffmpeg_analyzer')} python dev.py test --native-only\n"
        )
        sys.exit(1)

    if not _ffmpeg_analyzer_source_ready():
        print(
            "\nERROR: FFmpeg analyzer source tree is still incomplete after submodule update.\n"
            f"Expected: {FFMPEG_ANALYZER_DIR / 'configure'}\n"
            f"Expected: {FFMPEG_ANALYZER_DIR / 'tools' / 'void_ffmpeg_analyzer.c'}\n"
            f"Try: git submodule update --init --recursive --checkout {FFMPEG_SUBMODULE_PATH} {ZSTD_SUBMODULE_PATH}\n"
        )
        sys.exit(1)


def _resolve_first_existing_path(candidates: list[str | Path | None]) -> Path | None:
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate)
        if path.exists():
            return path.resolve()
    return None


def _resolve_command_path(name: str) -> Path | None:
    path = shutil.which(name)
    return Path(path).resolve() if path else None


def _resolve_msys_bash() -> Path:
    override = os.environ.get("MSYS2_BASH")
    bash = _resolve_first_existing_path([
        override,
        "C:/msys64/usr/bin/bash.exe",
        "C:/msys64/ucrt64/bin/bash.exe",
        _resolve_command_path("bash.exe"),
    ])
    if bash is None:
        raise RuntimeError(
            "MSYS2 bash not found. Install MSYS2 or set MSYS2_BASH to bash.exe."
        )
    return bash


def _resolve_nasm_exe() -> Path:
    nasm_path = os.environ.get("VOID_NASM")
    if nasm_path:
        path = Path(nasm_path)
        if path.exists():
            return path.resolve()
        raise RuntimeError(f"NASM not found at VOID_NASM: {nasm_path}")

    nasm_dir = os.environ.get("VOID_NASM_DIR")
    if nasm_dir:
        path = Path(nasm_dir) / "nasm.exe"
        if path.exists():
            return path.resolve()
        raise RuntimeError(f"NASM not found at VOID_NASM_DIR: {path}")

    nasm_candidates: list[str | Path | None] = [
        _resolve_command_path("nasm.exe"),
        "C:/Program Files/NASM/nasm.exe",
        "C:/Program Files (x86)/NASM/nasm.exe",
        "C:/msys64/usr/bin/nasm.exe",
        "C:/msys64/mingw64/bin/nasm.exe",
        "C:/msys64/ucrt64/bin/nasm.exe",
    ]
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        nasm_candidates.append(Path(local_app_data) / "bin" / "NASM" / "nasm.exe")

    nasm = _resolve_first_existing_path(nasm_candidates)
    if nasm is None:
        raise RuntimeError(
            "NASM not found. Install NASM and add nasm.exe to PATH, "
            "or set VOID_NASM / VOID_NASM_DIR."
        )
    return nasm


def _resolve_vcvars64() -> Path:
    override = os.environ.get("VOID_VCVARS64")
    if override:
        path = Path(override)
        if path.exists():
            return path.resolve()
        raise RuntimeError(f"Visual Studio vcvars64.bat not found at VOID_VCVARS64: {override}")

    candidates: list[Path] = []
    for root in (
        Path("C:/Program Files/Microsoft Visual Studio"),
        Path("C:/Program Files (x86)/Microsoft Visual Studio"),
    ):
        if root.exists():
            candidates.extend(root.rglob("vcvars64.bat"))
    if candidates:
        return sorted(candidates, key=lambda path: str(path), reverse=True)[0].resolve()

    raise RuntimeError("Visual Studio vcvars64.bat not found. Install Visual Studio Build Tools.")


def _to_msys_path(path: Path) -> str:
    text = str(path.resolve()).replace("\\", "/")
    if len(text) >= 2 and text[1] == ":":
        return f"/{text[0].lower()}{text[2:]}"
    return text


def _cmd_arg(value: str | Path) -> str:
    text = str(value)
    if not text:
        return '""'
    if any(char in text for char in ' \t"'):
        return '"' + text.replace('"', '""') + '"'
    return text


def _cmd_line(args: list[str | Path]) -> str:
    return " ".join(_cmd_arg(arg) for arg in args)


def _build_ffmpeg_analyzer_windows(*, clean: bool) -> None:
    if sys.platform != "win32":
        raise RuntimeError("FFmpeg analyzer build is only supported on Windows.")

    repo_root = FFMPEG_ANALYZER_DIR
    vendor_root = repo_root.parent
    zstd_root = vendor_root / "zstd"
    configuration = os.environ.get("VOID_FFMPEG_ANALYZER_CONFIGURATION", "analysis-minimal")
    build_dir = repo_root / "build" / f"windows-msvc-{configuration}"
    zstd_build_dir = build_dir / "zstd"
    output_dir = repo_root / "bin" / "windows-x64"

    msys_bash = _resolve_msys_bash()
    nasm_exe = _resolve_nasm_exe()
    vcvars64 = _resolve_vcvars64()

    zstd_cmake = zstd_root / "build" / "cmake" / "CMakeLists.txt"
    if not zstd_cmake.exists():
        raise RuntimeError(f"zstd source tree not found or incomplete: {zstd_root}")

    if clean and build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    repo_root_msys = _to_msys_path(repo_root)
    build_dir_msys = _to_msys_path(build_dir)
    nasm_dir_msys = _to_msys_path(nasm_exe.parent)
    zstd_include_msys = _to_msys_path(zstd_root / "lib")
    zstd_lib = zstd_build_dir / "lib" / "Release" / "zstd_static.lib"
    zstd_lib_msys = _to_msys_path(zstd_lib)
    bash_file = build_dir / "build_voidplayer.sh"
    bash_file_msys = _to_msys_path(bash_file)
    cmd_file = build_dir / "build_voidplayer.cmd"

    configure_command = f"""\"{repo_root_msys}/configure\" \\
  --toolchain=msvc \\
  --arch=x86_64 \\
  --target-os=win64 \\
  --disable-shared \\
  --enable-static \\
  --disable-programs \\
  --disable-doc \\
  --disable-avdevice \\
  --disable-avfilter \\
  --disable-swresample \\
  --disable-swscale \\
  --disable-everything \\
  --extra-cflags=\"-DVOIDPLAYER_VACHUNK_ZSTD=1 -I{zstd_include_msys}\" \\
  --extra-ldexeflags=\"{zstd_lib_msys}\" \\
  --enable-decoder=vvc,hevc,h264,av1,vp9,mpeg2video \\
  --enable-parser=vvc,hevc,h264,av1,vp9,mpegvideo \\
  --enable-demuxer=mov,matroska,flv,vvc,hevc,h264,ivf,mpegvideo,mpegts \\
  --enable-bsf=vvc_mp4toannexb,hevc_mp4toannexb,h264_mp4toannexb \\
  --enable-protocol=file"""

    bash_file.write_text(f"""set -euo pipefail
export MSYSTEM=UCRT64
export CHERE_INVOKING=1
export PATH="/usr/bin:{nasm_dir_msys}:$PATH"
command -v cl.exe
command -v make
command -v nasm.exe || command -v nasm
cd "{build_dir_msys}"
{configure_command}
rm -f \\
      libavformat/allformats.o libavformat/allformats.d \\
      libavcodec/allcodecs.o libavcodec/allcodecs.d \\
      libavcodec/bitstream_filters.o libavcodec/bitstream_filters.d \\
      libavcodec/cbs.o libavcodec/cbs.d \\
      libavcodec/codec_list.o libavcodec/codec_list.d \\
      libavcodec/parser_list.o libavcodec/parser_list.d \\
      libavcodec/bsf_list.o libavcodec/bsf_list.d
make -j$(nproc) tools/void_ffmpeg_analyzer.exe
""", encoding="ascii")

    zstd_configure_args: list[str | Path] = [
        "cmake",
        "-S",
        zstd_root / "build" / "cmake",
        "-B",
        zstd_build_dir,
    ]
    cmake_generator = os.environ.get("CMAKE_GENERATOR")
    cmake_platform = os.environ.get("CMAKE_GENERATOR_PLATFORM", "x64")
    if cmake_generator:
        zstd_configure_args.extend(["-G", cmake_generator])
    if cmake_platform:
        zstd_configure_args.extend(["-A", cmake_platform])
    zstd_configure_args.extend([
        "-DZSTD_BUILD_SHARED=OFF",
        "-DZSTD_BUILD_STATIC=ON",
        "-DZSTD_BUILD_PROGRAMS=OFF",
        "-DZSTD_BUILD_TESTS=OFF",
        "-DZSTD_BUILD_CONTRIB=OFF",
        "-DZSTD_LEGACY_SUPPORT=OFF",
        "-DZSTD_MULTITHREAD_SUPPORT=OFF",
        "-DZSTD_USE_STATIC_RUNTIME=ON",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    ])

    cmd_file.write_text(f"""@echo off
call {_cmd_arg(vcvars64)}
if errorlevel 1 exit /b %errorlevel%
{_cmd_line(zstd_configure_args)}
if errorlevel 1 exit /b %errorlevel%
cmake --build {_cmd_arg(zstd_build_dir)} --config Release --target libzstd_static
if errorlevel 1 exit /b %errorlevel%
{_cmd_arg(msys_bash)} {_cmd_arg(bash_file_msys)}
""", encoding="ascii")

    run(["cmd.exe", "/d", "/s", "/c", str(cmd_file)], cwd=str(repo_root))

    built_exe = build_dir / "tools" / "void_ffmpeg_analyzer.exe"
    if not built_exe.exists():
        raise RuntimeError(f"Expected analyzer was not produced: {built_exe}")

    dest = output_dir / "void_ffmpeg_analyzer.exe"
    shutil.copy2(built_exe, dest)
    print(f"Installed analyzer to {dest}")


def _find_zstd_static_lib(zstd_build_dir: Path) -> Path:
    candidates = [
        zstd_build_dir / "lib" / "Release" / "zstd_static.lib",
        zstd_build_dir / "lib" / "libzstd.a",
        zstd_build_dir / "libzstd.a",
        zstd_build_dir / "Release" / "zstd_static.lib",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    matches = sorted(zstd_build_dir.rglob("libzstd.a"))
    if matches:
        return matches[0]
    matches = sorted(zstd_build_dir.rglob("zstd_static.lib"))
    if matches:
        return matches[0]
    raise RuntimeError(f"zstd static library was not produced under {zstd_build_dir}")


def _patch_ffmpeg_analyzer_makefile_for_macos(repo_root: Path) -> tuple[Path, str]:
    makefile = repo_root / "tools" / "Makefile"
    text = makefile.read_text(encoding="utf-8")
    old = (
        "tools/void_ffmpeg_analyzer$(EXESUF): tools/void_ffmpeg_analyzer.o "
        "libavformat/avformat.lib libavcodec/avcodec.lib libavutil/avutil.lib\n"
        "tools/void_ffmpeg_analyzer$(EXESUF): ELIBS = "
        "libavformat/avformat.lib libavcodec/avcodec.lib libavutil/avutil.lib "
        "$(EXTRALIBS-avformat) $(EXTRALIBS-avcodec) $(EXTRALIBS-avutil)\n"
    )
    new = (
        "tools/void_ffmpeg_analyzer$(EXESUF): tools/void_ffmpeg_analyzer.o "
        "libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a\n"
        "tools/void_ffmpeg_analyzer$(EXESUF): ELIBS = "
        "libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a "
        "$(EXTRALIBS-avformat) $(EXTRALIBS-avcodec) $(EXTRALIBS-avutil)\n"
    )
    if old not in text:
        raise RuntimeError(f"Unexpected analyzer Makefile shape: {makefile}")
    makefile.write_text(text.replace(old, new), encoding="utf-8")
    return makefile, text


def _build_ffmpeg_analyzer_macos(*, clean: bool) -> None:
    if sys.platform != "darwin":
        raise RuntimeError("FFmpeg analyzer macOS build is only supported on macOS.")

    repo_root = FFMPEG_ANALYZER_DIR
    vendor_root = repo_root.parent
    zstd_root = vendor_root / "zstd"
    machine = platform.machine().lower()
    arch = "aarch64" if machine in ("arm64", "aarch64") else "x86_64"
    configuration = os.environ.get("VOID_FFMPEG_ANALYZER_CONFIGURATION", "analysis-minimal")
    build_dir = repo_root / "build" / f"{host_platform_id()}-{configuration}"
    zstd_build_dir = build_dir / "zstd"
    output_dir = repo_root / "bin" / host_platform_id()

    zstd_cmake = zstd_root / "build" / "cmake" / "CMakeLists.txt"
    if not zstd_cmake.exists():
        raise RuntimeError(f"zstd source tree not found or incomplete: {zstd_root}")

    if clean and build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)
    output_dir.mkdir(parents=True, exist_ok=True)

    zstd_configure_args: list[str | Path] = [
        "cmake",
        "-S",
        zstd_root / "build" / "cmake",
        "-B",
        zstd_build_dir,
        "-DZSTD_BUILD_SHARED=OFF",
        "-DZSTD_BUILD_STATIC=ON",
        "-DZSTD_BUILD_PROGRAMS=OFF",
        "-DZSTD_BUILD_TESTS=OFF",
        "-DZSTD_BUILD_CONTRIB=OFF",
        "-DZSTD_LEGACY_SUPPORT=OFF",
        "-DZSTD_MULTITHREAD_SUPPORT=OFF",
    ]
    run([str(arg) for arg in zstd_configure_args], cwd=str(repo_root))
    run([
        "cmake",
        "--build",
        str(zstd_build_dir),
        "--target",
        "libzstd_static",
        "--",
        f"-j{os.cpu_count() or 4}",
    ], cwd=str(repo_root))
    zstd_lib = _find_zstd_static_lib(zstd_build_dir)

    configure_cmd = [
        str(repo_root / "configure"),
        f"--arch={arch}",
        "--target-os=darwin",
        "--cc=clang",
        "--disable-shared",
        "--enable-static",
        "--enable-pic",
        "--disable-programs",
        "--disable-doc",
        "--disable-avdevice",
        "--disable-avfilter",
        "--disable-swresample",
        "--disable-swscale",
        "--disable-everything",
        f"--extra-cflags=-DVOIDPLAYER_VACHUNK_ZSTD=1 -I{zstd_root / 'lib'}",
        f"--extra-ldexeflags={zstd_lib}",
        "--enable-decoder=vvc,hevc,h264,av1,vp9,mpeg2video",
        "--enable-parser=vvc,hevc,h264,av1,vp9,mpegvideo",
        "--enable-demuxer=mov,matroska,flv,vvc,hevc,h264,ivf,mpegvideo,mpegts",
        "--enable-bsf=vvc_mp4toannexb,hevc_mp4toannexb,h264_mp4toannexb",
        "--enable-protocol=file",
    ]
    if arch == "x86_64":
        configure_cmd.append("--disable-x86asm")

    run(configure_cmd, cwd=str(build_dir))
    patched_makefile, original_makefile = _patch_ffmpeg_analyzer_makefile_for_macos(repo_root)
    try:
        run([
            "make",
            f"-j{os.cpu_count() or 4}",
            "tools/void_ffmpeg_analyzer",
        ], cwd=str(build_dir))
    finally:
        patched_makefile.write_text(original_makefile, encoding="utf-8")

    built_tool = build_dir / "tools" / "void_ffmpeg_analyzer"
    if not built_tool.exists():
        raise RuntimeError(f"Expected analyzer was not produced: {built_tool}")

    dest = output_dir / "void_ffmpeg_analyzer"
    shutil.copy2(built_tool, dest)
    dest.chmod(dest.stat().st_mode | 0o111)
    print(f"Installed analyzer to {dest}")


def _build_ffmpeg_analyzer(*, clean: bool) -> None:
    if sys.platform == "win32":
        _build_ffmpeg_analyzer_windows(clean=clean)
        return
    if sys.platform == "darwin":
        _build_ffmpeg_analyzer_macos(clean=clean)
        return
    raise RuntimeError(f"FFmpeg analyzer build is not supported on {sys.platform}.")


def ensure_ffmpeg_analyzer_tool() -> None:
    """Prepare FFmpeg analyzer for H.264/H.265/VVC VACache overlay generation."""
    analyzer = find_ffmpeg_analyzer()
    if os.environ.get("VOID_FFMPEG_ANALYZER"):
        if not analyzer.exists():
            print(f"ERROR: VOID_FFMPEG_ANALYZER does not exist: {analyzer}")
            sys.exit(1)
        _copy_ffmpeg_analyzer_to_vendor_bin(analyzer)
        return

    if _env_flag("VOIDPLAYER_ALLOW_MISSING_FFMPEG_ANALYZER"):
        if analyzer.exists():
            return
        print(
            "WARNING: VOIDPLAYER_ALLOW_MISSING_FFMPEG_ANALYZER=1; "
            "continuing without bundled FFmpeg analyzer."
        )
        return

    _ensure_ffmpeg_analyzer_submodule()

    stamp = _ffmpeg_analyzer_stamp_path()
    if not _tool_needs_rebuild(
        analyzer,
        stamp,
        _ffmpeg_analyzer_signature(),
        "FFmpeg analyzer",
    ):
        return

    header("Prepare FFmpeg analyzer for H.264/H.265/VVC analysis")
    print("Building FFmpeg analyzer before app/native build...")
    print(f"Tip: set VOID_FFMPEG_ANALYZER=/path/to/{executable_name('void_ffmpeg_analyzer')} to reuse a prebuilt analyzer.")
    try:
        _build_ffmpeg_analyzer(clean=True)
    except subprocess.CalledProcessError:
        print("\nERROR: FFmpeg analyzer build failed.")
        sys.exit(1)
    except RuntimeError as exc:
        print(f"\nERROR: {exc}")
        sys.exit(1)

    analyzer = find_ffmpeg_analyzer()
    if not analyzer.exists():
        print(f"ERROR: FFmpeg analyzer was not found after build: {analyzer}")
        sys.exit(1)
    _copy_ffmpeg_analyzer_to_vendor_bin(analyzer)
    _write_ffmpeg_analyzer_stamp(analyzer)


def _copy_ffmpeg_analyzer_to_vendor_bin(analyzer: Path) -> None:
    vendor_bin = FFMPEG_ANALYZER_DIR / "bin" / host_platform_id()
    expected = vendor_bin / executable_name("void_ffmpeg_analyzer")
    if analyzer.resolve() == expected.resolve():
        return

    vendor_bin.mkdir(parents=True, exist_ok=True)
    shutil.copy2(analyzer, expected)
    if sys.platform != "win32":
        expected.chmod(expected.stat().st_mode | 0o111)
    print(f"Copied FFmpeg analyzer to {expected}")


def native_build(debug: bool, test: bool = True, github: bool = False) -> None:
    """Build native standalone module, optionally run tests."""
    build_type = "Debug" if debug else "Release"

    if not github:
        ensure_analysis_test_tools()
    else:
        print("GitHub native mode: skipping external analysis tool builds.")

    header(f"Build native standalone ({build_type})")
    build_cmd = [sys.executable, "-u", str(NATIVE_BUILD_PY), "--build-only"]
    if debug:
        build_cmd.append("--debug")
    if github:
        build_cmd.append("--github")
    if _env_flag("VOID_USE_LOCAL_DEPS"):
        build_cmd.append("--use-local-deps")
    run(build_cmd, cwd=str(NATIVE_DIR))

    if test and github:
        print("GitHub native mode: skipping executable tests on hosted runners.")
        return

    if test:
        header(f"Test native standalone ({build_type})")
        test_cmd = [sys.executable, "-u", str(NATIVE_BUILD_PY), "--test-only"]
        if debug:
            test_cmd.append("--debug")
        run(test_cmd, cwd=str(NATIVE_DIR))


def native_build_macos(debug: bool, test: bool = True, github: bool = False) -> None:
    """Build the portable native macOS targets, optionally run CTest."""
    build_type = "Debug" if debug else "Release"
    build_dir = MACOS_NATIVE_MAKE_BUILD_DIR
    parallelism = str(os.cpu_count() or 4)

    header(f"Configure native macOS ({build_type})")
    run([
        "cmake",
        "-S",
        str(NATIVE_DIR),
        "-B",
        str(build_dir),
        "-G",
        "Unix Makefiles",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        "-DBUILD_ANALYSIS=OFF",
        f"-DBUILD_TESTS={'ON' if test else 'OFF'}",
        "-DBUILD_PYTHON=OFF",
        f"-DFFMPEG_ROOT={MACOS_FFMPEG_ROOT}",
        *(
            ["-DVOID_USE_LOCAL_DEPS=ON"]
            if _env_flag("VOID_USE_LOCAL_DEPS")
            else []
        ),
    ], cwd=str(ROOT))

    header(f"Build native macOS ({build_type})")
    run([
        "cmake",
        "--build",
        str(build_dir),
        "--",
        f"-j{parallelism}",
    ], cwd=str(ROOT))

    if test:
        header(f"Test native macOS ({build_type})")
        ctest_cmd = [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--output-on-failure",
        ]
        if github:
            ctest_cmd.extend(["-LE", "hosted-flaky|videotoolbox"])
        run(ctest_cmd, cwd=str(ROOT))
