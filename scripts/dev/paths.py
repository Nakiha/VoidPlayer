"""Shared paths for VoidPlayer development commands."""

import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

NATIVE_DIR = ROOT / "native"
NATIVE_BUILD_PY = NATIVE_DIR / "build.py"
NATIVE_BUILD_DIR = NATIVE_DIR / "build-msvc"
DEMO_SCRIPT = NATIVE_DIR / "video_renderer" / "demo" / "demo_video_renderer.py"

FFMPEG_ANALYZER_DIR = ROOT / "native" / "analysis" / "vendor" / "ffmpeg"
FFMPEG_ANALYZER_BUILD_SCRIPT = FFMPEG_ANALYZER_DIR / "voidplayer" / "build_windows_msvc.ps1"


def find_ffmpeg_analyzer() -> Path:
    """Find the instrumented FFmpeg analyzer used for H.264/H.265/VVC overlays."""
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


def macos_app_bundle_path(debug: bool) -> Path:
    build_type = "Debug" if debug else "Release"
    return (
        ROOT
        / "build"
        / "macos"
        / "Build"
        / "Products"
        / build_type
        / "VoidPlayer.app"
    )


def macos_app_exe_path(debug: bool) -> Path:
    return macos_app_bundle_path(debug) / "Contents" / "MacOS" / "VoidPlayer"
