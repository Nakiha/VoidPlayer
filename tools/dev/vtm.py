"""VTM DecoderApp build and analysis commands."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

from .paths import ROOT, VTM_BUILD_DIR, VTM_DECODER, VTM_DIR
from .process import header, run


def ensure_submodule() -> None:
    """Ensure tools/vtm submodule is initialized and on voidplayer-patches."""
    if not (VTM_DIR / ".git").exists():
        print("VTM submodule not initialized. Running git submodule update...")
        run(["git", "submodule", "update", "--init", "--remote", "tools/vtm"], cwd=str(ROOT))

    result = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        capture_output=True,
        text=True,
        cwd=str(VTM_DIR),
    )
    branch = result.stdout.strip()
    if branch != "voidplayer-patches":
        print(f"WARNING: VTM submodule on branch '{branch}', expected 'voidplayer-patches'")


def extract_raw_vvc(video_path: Path) -> Path:
    """Extract raw VVC bitstream from container (mp4/mkv/etc)."""
    raw_path = video_path.with_suffix(".vvc")
    if raw_path.exists():
        print(f"  Reusing existing raw bitstream: {raw_path}")
        return raw_path

    bundled_ffmpeg = ROOT / "windows" / "libs" / "ffmpeg" / "bin" / "ffmpeg.exe"
    ffmpeg_cmd = str(bundled_ffmpeg) if bundled_ffmpeg.exists() else "ffmpeg"

    print(f"  Extracting raw VVC bitstream from {video_path.name}...")
    run([
        ffmpeg_cmd, "-y", "-i", str(video_path),
        "-c:v", "copy", "-bsf:v", "vvc_mp4toannexb",
        "-f", "rawvideo", str(raw_path),
    ], cwd=str(ROOT))
    return raw_path


def cmd_vtm(args) -> None:
    """Build VTM DecoderApp or generate binary stats."""
    ensure_submodule()

    if args.vtm_action == "build":
        cmd_vtm_build()
    elif args.vtm_action == "analyze":
        if not args.video:
            print("ERROR: 'vtm analyze' requires a video file path")
            sys.exit(1)
        cmd_vtm_analyze(args)
    else:
        print(f"Unknown vtm action: {args.vtm_action}")
        sys.exit(1)


def cmd_vtm_build() -> None:
    """Build VTM DecoderApp with MSVC (static runtime, no DLL dependencies)."""
    cache = VTM_BUILD_DIR / "CMakeCache.txt"
    if cache.exists():
        content = cache.read_text(errors="ignore")
        if "MinGW" in content:
            print("  Cleaning old MinGW build...")
            shutil.rmtree(VTM_BUILD_DIR)

    VTM_BUILD_DIR.mkdir(parents=True, exist_ok=True)

    header("Configure VTM (MSVC, static runtime)")
    run([
        "cmake", "-B", str(VTM_BUILD_DIR), "-S", str(VTM_DIR),
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
        "-DCMAKE_CXX_FLAGS=/wd4819",
    ], cwd=str(ROOT))

    header("Build VTM DecoderApp (Release)")
    run([
        "cmake", "--build", str(VTM_BUILD_DIR), "--config", "Release", "--target", "DecoderApp",
    ], cwd=str(ROOT))

    if VTM_DECODER.exists():
        size_mb = VTM_DECODER.stat().st_size / (1024 * 1024)
        print(f"\n  DecoderApp built: {VTM_DECODER} ({size_mb:.1f} MB)")
    else:
        print(f"\n  WARNING: DecoderApp not found at {VTM_DECODER}")


def cmd_vtm_analyze(args) -> None:
    """Generate .vbs2 binary stats for a video file."""
    if not VTM_DECODER.exists():
        print("DecoderApp not found. Building first...")
        cmd_vtm_build()

    video_path = Path(args.video).resolve()
    if not video_path.exists():
        print(f"ERROR: video not found: {video_path}")
        sys.exit(1)

    vbs2_path = video_path.with_suffix(".vbs2")
    raw_path = extract_raw_vvc(video_path)

    header(f"Generate VBS2 stats for {video_path.name}")
    print(f"  Output: {vbs2_path}")

    env = os.environ.copy()
    env["VTM_BINARY_STATS"] = str(vbs2_path)

    run([
        str(VTM_DECODER),
        "-b", str(raw_path),
        "--TraceFile=NUL",
        "--TraceRule=D_BLOCK_STATISTICS_CODED:poc>=0",
        "-o", "NUL",
    ], cwd=str(ROOT), env=env)

    if vbs2_path.exists():
        size_mb = vbs2_path.stat().st_size / (1024 * 1024)
        print(f"\n  Done: {vbs2_path} ({size_mb:.1f} MB)")
    else:
        print(f"\n  ERROR: output file not created: {vbs2_path}")
        sys.exit(1)
