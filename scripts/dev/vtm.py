"""VTM DecoderApp build and analysis commands."""

import os
import shutil
import subprocess
import sys
from pathlib import Path

from .paths import ROOT, VTM_ANALYSIS_DIR, VTM_BUILD_DIR, VTM_DIR, find_vtm_decoder
from .process import header, run

VTM_SUBMODULE_PATH = "native/analysis/vendor/vtm"


def vtm_source_ready() -> bool:
    """Return whether the VTM source checkout looks usable."""
    return (VTM_DIR / "CMakeLists.txt").is_file()


def ensure_submodule() -> None:
    """Ensure the analysis VTM submodule has a usable source checkout."""
    if not vtm_source_ready():
        print("VTM source tree missing or incomplete. Running git submodule update...")
        run([
            "git", "submodule", "update", "--init", "--recursive", "--checkout",
            VTM_SUBMODULE_PATH,
        ], cwd=str(ROOT))

    if not vtm_source_ready():
        print(
            "\nERROR: VTM source tree is still incomplete after submodule update.\n"
            f"Expected: {VTM_DIR / 'CMakeLists.txt'}\n"
            "Try one of:\n"
            f"  git submodule update --init --recursive --checkout {VTM_SUBMODULE_PATH}\n"
            "  set VTM_DECODER_APP=C:\\path\\to\\DecoderApp.exe\n"
        )
        sys.exit(1)

    result = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        capture_output=True,
        text=True,
        cwd=str(VTM_DIR),
    )
    branch = result.stdout.strip()
    if branch not in ("voidplayer-patches", "HEAD"):
        print(f"WARNING: VTM submodule on branch '{branch}', expected 'voidplayer-patches'")


def is_relative_to(path: Path, parent: Path) -> bool:
    """Return whether path is inside parent, compatible with older Python."""
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def analysis_output_dir(video_path: Path) -> Path:
    """Choose where generated VTM artifacts should be written.

    Repository fixtures under resources/ are read-only by convention. When a
    fixture is analyzed directly, keep generated VVC/vbs3 artifacts under
    build/. If the caller already copied the video to a temp directory, write
    alongside that temp input so the caller can clean the whole directory.
    """
    resources_dir = ROOT / "resources"
    if is_relative_to(video_path, resources_dir):
        return VTM_ANALYSIS_DIR / video_path.stem
    return video_path.parent


def extract_raw_vvc(video_path: Path, output_dir: Path) -> Path:
    """Extract raw VVC bitstream from container (mp4/mkv/etc)."""
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = output_dir / f"{video_path.stem}.vvc"
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
    ensure_submodule()

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

    decoder = find_vtm_decoder()
    if decoder.exists():
        size_mb = decoder.stat().st_size / (1024 * 1024)
        print(f"\n  DecoderApp built: {decoder} ({size_mb:.1f} MB)")
    else:
        print(f"\n  WARNING: DecoderApp not found under {VTM_DIR / 'bin'}")


def cmd_vtm_analyze(args) -> None:
    """Generate VBS binary stats for a video file."""
    decoder = find_vtm_decoder()
    if not decoder.exists():
        print("DecoderApp not found. Building first...")
        cmd_vtm_build()
        decoder = find_vtm_decoder()

    if not decoder.exists():
        print(f"ERROR: DecoderApp not found under {VTM_DIR / 'bin'}")
        sys.exit(1)

    video_path = Path(args.video).resolve()
    if not video_path.exists():
        print(f"ERROR: video not found: {video_path}")
        sys.exit(1)

    output_dir = analysis_output_dir(video_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    stats_format = getattr(args, "format", "vbs3")
    stats_path = output_dir / f"{video_path.stem}.{stats_format}"
    raw_path = extract_raw_vvc(video_path, output_dir)

    header(f"Generate {stats_format.upper()} stats for {video_path.name}")
    print(f"  Artifact dir: {output_dir}")
    print(f"  Output: {stats_path}")

    env = os.environ.copy()
    env["VTM_BINARY_STATS"] = str(stats_path)
    env["VTM_BINARY_STATS_FORMAT"] = stats_format.upper()

    run([
        str(decoder),
        "-b", str(raw_path),
        "--TraceFile=NUL",
        "--TraceRule=D_BLOCK_STATISTICS_CODED:poc>=0",
        "-o", "NUL",
    ], cwd=str(ROOT), env=env)

    if stats_path.exists():
        size_mb = stats_path.stat().st_size / (1024 * 1024)
        print(f"\n  Done: {stats_path} ({size_mb:.1f} MB)")
    else:
        print(f"\n  ERROR: output file not created: {stats_path}")
        sys.exit(1)
