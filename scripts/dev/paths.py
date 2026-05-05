"""Shared paths for VoidPlayer development commands."""

import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

NATIVE_DIR = ROOT / "native"
NATIVE_BUILD_PY = NATIVE_DIR / "build.py"
NATIVE_BUILD_DIR = NATIVE_DIR / "build-msvc"
DEMO_SCRIPT = NATIVE_DIR / "video_renderer" / "demo" / "demo_video_renderer.py"

VTM_DIR = ROOT / "native" / "analysis" / "vendor" / "vtm"
VTM_BUILD_DIR = VTM_DIR / "build"
VTM_ANALYSIS_DIR = ROOT / "build" / "vtm_analysis"
FFMPEG_ANALYZER_DIR = ROOT / "native" / "analysis" / "vendor" / "ffmpeg"
FFMPEG_ANALYZER_BUILD_SCRIPT = FFMPEG_ANALYZER_DIR / "voidplayer" / "build_windows_msvc.ps1"


def find_vtm_decoder() -> Path:
    """Find DecoderApp.exe under bin/vs*/; MSVC output varies by VS version."""
    override = os.environ.get("VTM_DECODER_APP")
    if override:
        return Path(override)

    bin_dir = VTM_DIR / "bin"
    if bin_dir.exists():
        for path in sorted(bin_dir.rglob("DecoderApp.exe"), reverse=True):
            return path
    return bin_dir / "DecoderApp.exe"


VTM_DECODER = find_vtm_decoder()


def find_ffmpeg_analyzer() -> Path:
    """Find the instrumented FFmpeg analyzer used for H.264/H.265 VBS4."""
    override = os.environ.get("VOID_FFMPEG_ANALYZER")
    if override:
        return Path(override)

    bin_dir = FFMPEG_ANALYZER_DIR / "bin" / "windows-x64"
    for name in ("void_ffmpeg_analyzer.exe", "void_hevc_analyzer.exe"):
        path = bin_dir / name
        if path.exists():
            return path
    return bin_dir / "void_ffmpeg_analyzer.exe"


FFMPEG_ANALYZER = find_ffmpeg_analyzer()

WINDOWS_BUILD_DIR = ROOT / "build" / "windows" / "x64" / "runner"
WINDOWS_PACKAGE_DIR = ROOT / "build" / "package" / "windows"
WINDOWS_PACKAGE_STAGE_DIR = WINDOWS_PACKAGE_DIR / "VoidPlayer"
WINDOWS_INSTALLER_DIR = WINDOWS_PACKAGE_DIR / "installer"
WINDOWS_INNO_SCRIPT = ROOT / "installer" / "windows" / "VoidPlayer.iss"
WINDOWS_RELEASE_DOCS_DIR = ROOT / "installer" / "windows" / "docs"


def app_exe_path(debug: bool) -> Path:
    build_type = "Debug" if debug else "Release"
    return WINDOWS_BUILD_DIR / build_type / "void_player.exe"
