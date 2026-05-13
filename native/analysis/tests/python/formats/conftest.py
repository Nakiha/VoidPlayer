"""Shared fixtures for analysis binary format validation tests.

Run: python -m pytest native/analysis/tests/python/formats -v
"""

import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[5]
VIDEO_DIR = ROOT / "resources" / "video"
TEST_VIDEO = VIDEO_DIR / "h266_10s_1920x1080.mp4"
OVERLAY_VIDEO = VIDEO_DIR / "h264_9s_1920x1080.mp4"
TEMP_DIR = Path(tempfile.gettempdir()) / "void_player_analysis_format_test"


def _analysis_generate_exe() -> Path:
    explicit = os.environ.get("VOID_ANALYSIS_GENERATE_EXE")
    if explicit:
        return Path(explicit)

    for build_dir in (ROOT / "native" / "build-msvc", ROOT / "windows" / "native" / "build-msvc"):
        for config in ("Release", "Debug"):
            candidate = build_dir / config / "analysis_generate.exe"
            if candidate.exists():
                return candidate
    build_dir = ROOT / "native" / "build-msvc"
    return build_dir / "Release" / "analysis_generate.exe"


def _ffmpeg_analyzer_exe() -> Path:
    explicit = os.environ.get("VOID_FFMPEG_ANALYZER")
    if explicit:
        return Path(explicit)
    return (
        ROOT
        / "native"
        / "analysis"
        / "vendor"
        / "ffmpeg"
        / "bin"
        / "windows-x64"
        / "void_ffmpeg_analyzer.exe"
    )


@pytest.fixture(scope="session")
def analysis_paths(request):
    request.addfinalizer(lambda: shutil.rmtree(TEMP_DIR, ignore_errors=True))

    if not TEST_VIDEO.exists():
        pytest.skip(f"Test video not found: {TEST_VIDEO}")
    if not OVERLAY_VIDEO.exists():
        pytest.skip(f"Overlay test video not found: {OVERLAY_VIDEO}")

    generator = _analysis_generate_exe()
    if not generator.exists():
        pytest.skip(
            "analysis_generate.exe not found. Run: python dev.py build --native"
        )
    analyzer = _ffmpeg_analyzer_exe()
    if not analyzer.exists():
        pytest.skip("void_ffmpeg_analyzer.exe not found. Run: python dev.py build --native")

    if TEMP_DIR.exists():
        shutil.rmtree(TEMP_DIR)
    TEMP_DIR.mkdir(parents=True)

    temp_video = TEMP_DIR / TEST_VIDEO.name
    shutil.copy2(TEST_VIDEO, temp_video)

    vbi_file = TEMP_DIR / f"{TEST_VIDEO.stem}.vbi"
    vbt_file = TEMP_DIR / f"{TEST_VIDEO.stem}.vbt"
    vbs4_file = TEMP_DIR / f"{TEST_VIDEO.stem}.vbs4"

    subprocess.check_call([
        str(generator),
        str(temp_video),
        str(vbi_file),
        str(vbt_file),
    ], cwd=ROOT)

    subprocess.check_call([
        str(analyzer),
        "--codec",
        "h264",
        "--input",
        str(OVERLAY_VIDEO),
        "--vbs4",
        str(vbs4_file),
    ], cwd=ROOT)

    paths = {
        "video": temp_video,
        "vbs4": vbs4_file,
        "vbi": vbi_file,
        "vbt": vbt_file,
    }
    for name, path in paths.items():
        if name == "video":
            continue
        assert path.exists(), f"Expected generated file missing: {path}"

    return paths
