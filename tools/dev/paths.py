"""Shared paths for VoidPlayer development commands."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

NATIVE_DIR = ROOT / "windows" / "native"
NATIVE_BUILD_PY = NATIVE_DIR / "build.py"
NATIVE_BUILD_DIR = NATIVE_DIR / "build-msvc"
DEMO_SCRIPT = NATIVE_DIR / "video_renderer" / "demo" / "demo_video_renderer.py"

VTM_DIR = ROOT / "tools" / "vtm"
VTM_BUILD_DIR = VTM_DIR / "build"


def find_vtm_decoder() -> Path:
    """Find DecoderApp.exe under bin/vs*/; MSVC output varies by VS version."""
    bin_dir = VTM_DIR / "bin"
    if bin_dir.exists():
        for path in sorted(bin_dir.rglob("DecoderApp.exe"), reverse=True):
            return path
    return bin_dir / "DecoderApp.exe"


VTM_DECODER = find_vtm_decoder()


def app_exe_path(debug: bool) -> Path:
    build_type = "Debug" if debug else "Release"
    return ROOT / "build" / "windows" / "x64" / "runner" / build_type / "void_player.exe"
