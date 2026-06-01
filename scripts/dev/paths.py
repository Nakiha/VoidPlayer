"""Shared paths for VoidPlayer development commands."""

import os
import platform
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

NATIVE_DIR = ROOT / "native"
NATIVE_BUILD_PY = NATIVE_DIR / "build.py"
DEMO_SCRIPT = NATIVE_DIR / "video_renderer" / "demo" / "demo_video_renderer.py"

FFMPEG_ANALYZER_DIR = ROOT / "native" / "analysis" / "vendor" / "ffmpeg"


def default_native_build_dir() -> Path:
    if sys.platform == "win32":
        return NATIVE_DIR / "build-msvc"
    if sys.platform == "darwin":
        return NATIVE_DIR / "build-macos"
    return NATIVE_DIR / "build-native"


NATIVE_BUILD_DIR = default_native_build_dir()


def host_platform_id() -> str:
    if sys.platform == "win32":
        return "windows-x64"
    if sys.platform == "darwin":
        machine = platform.machine().lower()
        if machine in ("arm64", "aarch64"):
            return "macos-arm64"
        return "macos-x64"
    return f"{sys.platform}-{platform.machine().lower() or 'unknown'}"


def executable_name(name: str) -> str:
    return f"{name}.exe" if sys.platform == "win32" else name


def find_ffmpeg_analyzer() -> Path:
    """Find the instrumented FFmpeg analyzer used for H.264/H.265/VVC overlays."""
    override = os.environ.get("VOID_FFMPEG_ANALYZER")
    if override:
        return Path(override)

    bin_dir = FFMPEG_ANALYZER_DIR / "bin" / host_platform_id()
    for name in (executable_name("void_ffmpeg_analyzer"), executable_name("void_hevc_analyzer")):
        path = bin_dir / name
        if path.exists():
            return path
    return bin_dir / executable_name("void_ffmpeg_analyzer")


FFMPEG_ANALYZER = find_ffmpeg_analyzer()

WINDOWS_BUILD_DIR = ROOT / "build" / "windows" / "x64" / "runner"
MACOS_NATIVE_ANALYSIS_BUILD_DIR = NATIVE_DIR / "build-macos-analysis"
WINDOWS_PACKAGE_DIR = ROOT / "build" / "package" / "windows"
WINDOWS_PACKAGE_STAGE_DIR = WINDOWS_PACKAGE_DIR / "VoidPlayer"
WINDOWS_INSTALLER_DIR = WINDOWS_PACKAGE_DIR / "installer"
WINDOWS_INNO_SCRIPT = ROOT / "installer" / "windows" / "VoidPlayer.iss"
WINDOWS_RELEASE_DOCS_DIR = ROOT / "installer" / "windows" / "docs"
MACOS_PACKAGE_DIR = ROOT / "build" / "package" / "macos"
MACOS_PACKAGE_STAGE_DIR = MACOS_PACKAGE_DIR / "VoidPlayer"
MACOS_INSTALLER_DIR = MACOS_PACKAGE_DIR / "installer"
MACOS_RELEASE_DOCS_DIR = ROOT / "installer" / "macos" / "docs"


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


def find_voidplayer_cli() -> Path:
    if sys.platform == "win32":
        release_cli = WINDOWS_BUILD_DIR / "Release" / "VoidPlayerCli.exe"
        native_cli = NATIVE_BUILD_DIR / "Release" / "VoidPlayerCli.exe"
        return release_cli if release_cli.exists() else native_cli
    if sys.platform == "darwin":
        for candidate in (
            MACOS_NATIVE_ANALYSIS_BUILD_DIR / "VoidPlayerCli",
            NATIVE_DIR / "build-macos-make" / "VoidPlayerCli",
            NATIVE_BUILD_DIR / "VoidPlayerCli",
        ):
            if candidate.exists():
                return candidate
        return MACOS_NATIVE_ANALYSIS_BUILD_DIR / "VoidPlayerCli"
    return NATIVE_DIR / "build" / executable_name("VoidPlayerCli")
