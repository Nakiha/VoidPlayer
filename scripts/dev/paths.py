"""Shared paths for VoidPlayer development commands."""

import os
import platform
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = ROOT / "build"

NATIVE_DIR = ROOT / "native"
NATIVE_BUILD_PY = NATIVE_DIR / "build.py"
DEMO_SCRIPT = NATIVE_DIR / "video_renderer" / "demo" / "demo_video_renderer.py"
NATIVE_BUILD_ROOT = BUILD_DIR / "native"
NATIVE_STANDALONE_BUILD_ROOT = NATIVE_BUILD_ROOT / "standalone"
NATIVE_RUNNER_BUILD_ROOT = NATIVE_BUILD_ROOT / "runner"
NATIVE_ANALYSIS_BUILD_ROOT = NATIVE_BUILD_ROOT / "analysis"

FFMPEG_ANALYZER_DIR = ROOT / "native" / "analysis" / "vendor" / "ffmpeg"


def default_native_build_dir() -> Path:
    if sys.platform == "win32":
        return NATIVE_STANDALONE_BUILD_ROOT / "windows-msvc"
    if sys.platform == "darwin":
        return NATIVE_STANDALONE_BUILD_ROOT / "macos"
    return NATIVE_STANDALONE_BUILD_ROOT / "portable"


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

WINDOWS_BUILD_DIR = BUILD_DIR / "windows" / "x64" / "runner"
MACOS_NATIVE_ANALYSIS_BUILD_DIR = NATIVE_ANALYSIS_BUILD_ROOT / "macos"
MACOS_NATIVE_MAKE_BUILD_DIR = NATIVE_STANDALONE_BUILD_ROOT / "macos-make"
MACOS_XCODE_NATIVE_BUILD_DIR = NATIVE_RUNNER_BUILD_ROOT / "macos-xcode"
WINDOWS_PACKAGE_DIR = BUILD_DIR / "package" / "windows"
WINDOWS_PACKAGE_STAGE_DIR = WINDOWS_PACKAGE_DIR / "VoidPlayer"
WINDOWS_INSTALLER_DIR = WINDOWS_PACKAGE_DIR / "installer"
WINDOWS_INNO_SCRIPT = ROOT / "installer" / "windows" / "VoidPlayer.iss"
WINDOWS_RELEASE_DOCS_DIR = ROOT / "installer" / "windows" / "docs"
MACOS_PACKAGE_DIR = BUILD_DIR / "package" / "macos"
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
            MACOS_NATIVE_MAKE_BUILD_DIR / "VoidPlayerCli",
            NATIVE_BUILD_DIR / "VoidPlayerCli",
            NATIVE_DIR / "build-macos-analysis" / "VoidPlayerCli",
            NATIVE_DIR / "build-macos-make" / "VoidPlayerCli",
            NATIVE_DIR / "build-macos" / "VoidPlayerCli",
        ):
            if candidate.exists():
                return candidate
        return MACOS_NATIVE_ANALYSIS_BUILD_DIR / "VoidPlayerCli"
    return NATIVE_DIR / "build" / executable_name("VoidPlayerCli")
