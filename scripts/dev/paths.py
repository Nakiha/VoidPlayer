"""Shared paths for VoidPlayer development commands."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

NATIVE_DIR = ROOT / "native"
NATIVE_BUILD_PY = NATIVE_DIR / "build.py"
NATIVE_BUILD_DIR = NATIVE_DIR / "build-msvc"
DEMO_SCRIPT = NATIVE_DIR / "video_renderer" / "demo" / "demo_video_renderer.py"

VTM_DIR = ROOT / "native" / "analysis" / "vendor" / "vtm"
VTM_BUILD_DIR = VTM_DIR / "build"


def find_vtm_decoder() -> Path:
    """Find DecoderApp.exe under bin/vs*/; MSVC output varies by VS version."""
    bin_dir = VTM_DIR / "bin"
    if bin_dir.exists():
        for path in sorted(bin_dir.rglob("DecoderApp.exe"), reverse=True):
            return path
    return bin_dir / "DecoderApp.exe"


VTM_DECODER = find_vtm_decoder()

WINDOWS_BUILD_DIR = ROOT / "build" / "windows" / "x64" / "runner"
WINDOWS_PACKAGE_DIR = ROOT / "build" / "package" / "windows"
WINDOWS_PACKAGE_STAGE_DIR = WINDOWS_PACKAGE_DIR / "VoidPlayer"
WINDOWS_INSTALLER_DIR = WINDOWS_PACKAGE_DIR / "installer"
WINDOWS_INNO_SCRIPT = ROOT / "installer" / "windows" / "VoidPlayer.iss"
WINDOWS_RELEASE_DOCS_DIR = ROOT / "installer" / "windows" / "docs"


def app_exe_path(debug: bool) -> Path:
    build_type = "Debug" if debug else "Release"
    return WINDOWS_BUILD_DIR / build_type / "void_player.exe"
