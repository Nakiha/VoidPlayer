"""Native standalone build and test commands."""

import shutil
import subprocess
import sys
from pathlib import Path

from .paths import (
    FFMPEG_ANALYZER_DIR,
    FFMPEG_ANALYZER_BUILD_SCRIPT,
    NATIVE_BUILD_PY,
    NATIVE_DIR,
    find_ffmpeg_analyzer,
    find_vtm_decoder,
)
from .process import header, run


def ensure_analysis_test_tools() -> None:
    """Prepare external tools required by native analysis tests."""
    ensure_vtm_decoder_tool()
    ensure_ffmpeg_analyzer_tool()


def ensure_vtm_decoder_tool() -> None:
    """Prepare VTM DecoderApp for VVC/VBS4 generation."""
    decoder = find_vtm_decoder()
    if decoder.exists():
        return

    header("Prepare VTM DecoderApp for analysis tests")
    print("VTM DecoderApp missing; building it before native tests...")
    print("Tip: set VTM_DECODER_APP=C:\\path\\to\\DecoderApp.exe to reuse a prebuilt VTM.")

    from .vtm import cmd_vtm_build, ensure_submodule

    ensure_submodule()
    cmd_vtm_build()

    decoder = find_vtm_decoder()
    if not decoder.exists():
        print(f"ERROR: VTM DecoderApp was not found after build: {decoder}")
        sys.exit(1)


def ensure_ffmpeg_analyzer_tool() -> None:
    """Prepare FFmpeg analyzer for H.264/H.265 VBS4 generation."""
    analyzer = find_ffmpeg_analyzer()
    if analyzer.exists():
        _copy_ffmpeg_analyzer_to_vendor_bin(analyzer)
        return

    header("Prepare FFmpeg analyzer for H.264/H.265 analysis")
    if not FFMPEG_ANALYZER_BUILD_SCRIPT.exists():
        print(f"ERROR: FFmpeg analyzer build script not found: {FFMPEG_ANALYZER_BUILD_SCRIPT}")
        sys.exit(1)

    print("FFmpeg analyzer missing; building it before app/native build...")
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


def _copy_ffmpeg_analyzer_to_vendor_bin(analyzer: Path) -> None:
    vendor_bin = FFMPEG_ANALYZER_DIR / "bin" / "windows-x64"
    expected = vendor_bin / "void_ffmpeg_analyzer.exe"
    if analyzer.resolve() == expected.resolve():
        return

    vendor_bin.mkdir(parents=True, exist_ok=True)
    shutil.copy2(analyzer, expected)
    print(f"Copied FFmpeg analyzer to {expected}")


def native_build(debug: bool, test: bool = True) -> None:
    """Build native standalone module, optionally run tests."""
    build_type = "Debug" if debug else "Release"

    if test:
        ensure_analysis_test_tools()

    header(f"Build native standalone ({build_type})")
    build_cmd = [sys.executable, "-u", str(NATIVE_BUILD_PY), "--build-only"]
    if debug:
        build_cmd.append("--debug")
    run(build_cmd, cwd=str(NATIVE_DIR))

    if test:
        header(f"Test native standalone ({build_type})")
        test_cmd = [sys.executable, "-u", str(NATIVE_BUILD_PY), "--test-only"]
        if debug:
            test_cmd.append("--debug")
        run(test_cmd, cwd=str(NATIVE_DIR))
