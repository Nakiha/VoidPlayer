"""Native standalone build and test commands."""

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from .paths import (
    FFMPEG_ANALYZER_DIR,
    FFMPEG_ANALYZER_BUILD_SCRIPT,
    NATIVE_BUILD_PY,
    NATIVE_DIR,
    ROOT,
    find_ffmpeg_analyzer,
)
from .process import header, run

TOOL_STAMP_VERSION = 1
FFMPEG_SUBMODULE_PATH = "native/analysis/vendor/ffmpeg"
ZSTD_SUBMODULE_PATH = "native/analysis/vendor/zstd"


def ensure_analysis_test_tools() -> None:
    """Prepare external tools required by native analysis tests."""
    ensure_ffmpeg_analyzer_tool()


def _file_sha256(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


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
    script_hash = (
        _file_sha256(FFMPEG_ANALYZER_BUILD_SCRIPT)
        if FFMPEG_ANALYZER_BUILD_SCRIPT.exists()
        else "missing"
    )
    source_hashes = {}
    for relative in (
        "tools/void_ffmpeg_analyzer.c",
        "libavcodec/voidplayer_vbs4.c",
        "libavcodec/voidplayer_vbs4.h",
    ):
        source = FFMPEG_ANALYZER_DIR / relative
        source_hashes[relative] = _file_sha256(source) if source.exists() else "missing"

    return {
        "version": TOOL_STAMP_VERSION,
        "tool": "ffmpeg-analyzer",
        "configuration": "analysis-minimal-msvc",
        "ffmpeg_head": _git_head(FFMPEG_ANALYZER_DIR),
        "zstd_head": _zstd_head(),
        "build_script_sha256": script_hash,
        "source_sha256": source_hashes,
    }


def _ffmpeg_analyzer_stamp_path() -> Path:
    return FFMPEG_ANALYZER_DIR / "build" / "voidplayer-analyzer-stamp.json"


def _write_ffmpeg_analyzer_stamp(analyzer: Path) -> None:
    _write_json(_ffmpeg_analyzer_stamp_path(), _ffmpeg_analyzer_signature())


def _ffmpeg_analyzer_source_ready() -> bool:
    zstd_cmake = FFMPEG_ANALYZER_DIR.parent / "zstd" / "build" / "cmake" / "CMakeLists.txt"
    return (
        FFMPEG_ANALYZER_BUILD_SCRIPT.is_file()
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
            "  set VOID_FFMPEG_ANALYZER=C:\\path\\to\\void_ffmpeg_analyzer.exe\n"
        )
        sys.exit(1)

    if not _ffmpeg_analyzer_source_ready():
        print(
            "\nERROR: FFmpeg analyzer source tree is still incomplete after submodule update.\n"
            f"Expected: {FFMPEG_ANALYZER_BUILD_SCRIPT}\n"
            f"Expected: {FFMPEG_ANALYZER_DIR / 'tools' / 'void_ffmpeg_analyzer.c'}\n"
            f"Try: git submodule update --init --recursive --checkout {FFMPEG_SUBMODULE_PATH} {ZSTD_SUBMODULE_PATH}\n"
        )
        sys.exit(1)


def ensure_ffmpeg_analyzer_tool() -> None:
    """Prepare FFmpeg analyzer for H.264/H.265 VACache overlay generation."""
    analyzer = find_ffmpeg_analyzer()
    if os.environ.get("VOID_FFMPEG_ANALYZER"):
        if not analyzer.exists():
            print(f"ERROR: VOID_FFMPEG_ANALYZER does not exist: {analyzer}")
            sys.exit(1)
        _copy_ffmpeg_analyzer_to_vendor_bin(analyzer)
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

    header("Prepare FFmpeg analyzer for H.264/H.265 analysis")
    print("Building FFmpeg analyzer before app/native build...")
    print("Tip: set VOID_FFMPEG_ANALYZER=C:\\path\\to\\void_ffmpeg_analyzer.exe to reuse a prebuilt analyzer.")
    cmd = [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(FFMPEG_ANALYZER_BUILD_SCRIPT),
    ]
    try:
        run(cmd, cwd=str(FFMPEG_ANALYZER_BUILD_SCRIPT.parent.parent))
    except subprocess.CalledProcessError:
        print("\nERROR: FFmpeg analyzer build failed.")
        sys.exit(1)

    analyzer = find_ffmpeg_analyzer()
    if not analyzer.exists():
        print(f"ERROR: FFmpeg analyzer was not found after build: {analyzer}")
        sys.exit(1)
    _copy_ffmpeg_analyzer_to_vendor_bin(analyzer)
    _write_ffmpeg_analyzer_stamp(analyzer)


def _copy_ffmpeg_analyzer_to_vendor_bin(analyzer: Path) -> None:
    vendor_bin = FFMPEG_ANALYZER_DIR / "bin" / "windows-x64"
    expected = vendor_bin / "void_ffmpeg_analyzer.exe"
    if analyzer.resolve() == expected.resolve():
        return

    vendor_bin.mkdir(parents=True, exist_ok=True)
    shutil.copy2(analyzer, expected)
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
