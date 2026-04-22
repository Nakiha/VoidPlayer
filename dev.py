"""VoidPlayer dev script — build, run, launch, demo, test from one entry point.

Usage examples: see python dev.py -h
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
NATIVE_DIR = ROOT / "windows" / "native"
NATIVE_BUILD_PY = NATIVE_DIR / "build.py"
NATIVE_BUILD_DIR = NATIVE_DIR / "build-msvc"
DEMO_SCRIPT = NATIVE_DIR / "demo" / "demo_video_renderer.py"

VTM_DIR = ROOT / "tools" / "vtm"
VTM_BUILD_DIR = VTM_DIR / "build"


def _find_vtm_decoder() -> Path:
    """Find DecoderApp.exe under bin/vs*/ — MSVC output varies by VS version."""
    bin_dir = VTM_DIR / "bin"
    if bin_dir.exists():
        for path in sorted(bin_dir.rglob("DecoderApp.exe"), reverse=True):
            return path
    return bin_dir / "DecoderApp.exe"


VTM_DECODER = _find_vtm_decoder()


def header(title: str):
    print(f"\n{'=' * 60}")
    print(f"  {title}")
    print("=" * 60)


def run(cmd, **kwargs):
    """Run a command and handle Windows .bat/.cmd launchers."""
    cmd = [str(part) for part in cmd]
    print(f"> {subprocess.list2cmdline(cmd)}")

    executable = shutil.which(cmd[0])
    if executable:
        cmd[0] = executable
        if Path(executable).suffix.lower() in {".bat", ".cmd"}:
            cmd = ["cmd.exe", "/c", *cmd]

    subprocess.check_call(cmd, **kwargs)


def flutter_build(debug: bool):
    """Build Flutter Windows app."""
    build_type = "Debug" if debug else "Release"
    header(f"Build Flutter ({build_type})")

    cmd = ["flutter", "build", "windows"]
    if debug:
        cmd.append("--debug")
    else:
        cmd.append("--release")

    run(cmd, cwd=str(ROOT))


def native_build(debug: bool, test: bool = True):
    """Build native standalone module, optionally run tests."""
    build_type = "Debug" if debug else "Release"

    header(f"Build native standalone ({build_type})")
    build_cmd = [sys.executable, str(NATIVE_BUILD_PY), "--build-only"]
    if debug:
        build_cmd.append("--debug")
    run(build_cmd, cwd=str(NATIVE_DIR))

    if test:
        header(f"Test native standalone ({build_type})")
        test_cmd = [sys.executable, str(NATIVE_BUILD_PY), "--test-only"]
        if debug:
            test_cmd.append("--debug")
        run(test_cmd, cwd=str(NATIVE_DIR))


def app_exe_path(debug: bool) -> Path:
    build_type = "Debug" if debug else "Release"
    return ROOT / "build" / "windows" / "x64" / "runner" / build_type / "void_player.exe"


# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

def cmd_build(args):
    """Build native standalone module and/or Flutter app."""
    if args.native and args.flutter:
        print("ERROR: --native and --flutter are mutually exclusive")
        sys.exit(1)

    if not args.flutter:
        native_build(args.debug, test=not args.no_test)

    if not args.native:
        flutter_build(args.debug)

    print("\nBuild done.")


# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------

def cmd_run(args):
    """Run the Flutter application via flutter run."""
    flutter_args = ["flutter", "run", "-d", "windows"]
    flutter_args.append("--debug" if args.debug else "--release")

    if args.log_level:
        flutter_args.extend(["--", f"--log-level={args.log_level}"])

    header(f"Run Flutter ({'debug' if args.debug else 'release'})")
    run(flutter_args, cwd=str(ROOT))


# ---------------------------------------------------------------------------
# launch — launch exe directly, optionally build Flutter first
# ---------------------------------------------------------------------------

def cmd_launch(args):
    """Launch exe directly; build Flutter first only if requested or missing."""
    exe = app_exe_path(args.debug)

    if args.build or not exe.exists():
        flutter_build(args.debug)

    if not exe.exists():
        print(f"ERROR: exe not found: {exe}")
        sys.exit(1)

    cmd = [str(exe)]
    if args.log_level:
        cmd.append(f"--log-level={args.log_level}")
    if args.test_script:
        cmd.extend(["--test-script", str(Path(args.test_script).resolve())])

    header(f"Launch {exe}")
    subprocess.call(cmd)


# ---------------------------------------------------------------------------
# demo
# ---------------------------------------------------------------------------

def cmd_demo(args):
    """Run the native Python demo (PySide6 + video_renderer_native)."""
    build_type = "Debug" if args.debug else "Release"
    native_lib = NATIVE_BUILD_DIR / build_type / "video_renderer_native.pyd"

    if args.build or not native_lib.exists():
        native_build(args.debug, test=False)

    demo_cmd = [sys.executable, str(DEMO_SCRIPT)]
    demo_cmd.extend(str(video) for video in args.videos)

    env = os.environ.copy()
    if args.log_level:
        env["SPDLOG_LEVEL"] = args.log_level

    header(f"Run native demo ({build_type})")
    run(demo_cmd, cwd=str(NATIVE_DIR), env=env)


# ---------------------------------------------------------------------------
# test
# ---------------------------------------------------------------------------

def cmd_test(args):
    """Build and run native standalone tests."""
    native_build(args.debug, test=True)


# ---------------------------------------------------------------------------
# vtm — VTM DecoderApp build & analyze
# ---------------------------------------------------------------------------

def _ensure_submodule():
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


def _extract_raw_vvc(video_path: Path) -> Path:
    """Extract raw VVC bitstream from container (mp4/mkv/etc)."""
    raw_path = video_path.with_suffix(".vvc")
    if raw_path.exists():
        print(f"  Reusing existing raw bitstream: {raw_path}")
        return raw_path

    print(f"  Extracting raw VVC bitstream from {video_path.name}...")
    run([
        "ffmpeg", "-y", "-i", str(video_path),
        "-c:v", "copy", "-bsf:v", "vvc_mp4toannexb",
        "-f", "rawvideo", str(raw_path),
    ], cwd=str(ROOT))
    return raw_path


def cmd_vtm(args):
    """Build VTM DecoderApp or generate binary stats."""
    _ensure_submodule()

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


def cmd_vtm_build():
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
        "cmake", "--build", str(VTM_BUILD_DIR), "--config", "Release", "--target", "DecoderApp"
    ], cwd=str(ROOT))

    if VTM_DECODER.exists():
        size_mb = VTM_DECODER.stat().st_size / (1024 * 1024)
        print(f"\n  DecoderApp built: {VTM_DECODER} ({size_mb:.1f} MB)")
    else:
        print(f"\n  WARNING: DecoderApp not found at {VTM_DECODER}")


def cmd_vtm_analyze(args):
    """Generate .vbs2 binary stats for a video file."""
    if not VTM_DECODER.exists():
        print("DecoderApp not found. Building first...")
        cmd_vtm_build()

    video_path = Path(args.video).resolve()
    if not video_path.exists():
        print(f"ERROR: video not found: {video_path}")
        sys.exit(1)

    vbs2_path = video_path.with_suffix(".vbs2")
    raw_path = _extract_raw_vvc(video_path)

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


# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="VoidPlayer dev script",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  python dev.py
  python dev.py build
  python dev.py build --flutter
  python dev.py run
  python dev.py launch
  python dev.py launch --build
  python dev.py demo
  python dev.py test
  python dev.py vtm build
  python dev.py vtm analyze video.mp4
""",
    )
    sub = parser.add_subparsers(dest="command")

    # --- build ---
    p_build = sub.add_parser("build", help="Build native standalone and/or Flutter app")
    p_build.add_argument("--debug", action="store_true", help="Debug build")
    p_build.add_argument("--native", action="store_true", help="Build native standalone only")
    p_build.add_argument("--flutter", action="store_true", help="Build Flutter app only")
    p_build.add_argument("--no-test", action="store_true", help="Skip native standalone tests")

    # --- run ---
    p_run = sub.add_parser("run", help="Run Flutter app via flutter run")
    p_run.add_argument("--debug", action="store_true", help="Debug mode (hot reload)")
    p_run.add_argument("--log-level", type=str, default=None,
                       help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")

    # --- launch ---
    p_launch = sub.add_parser("launch", help="Launch exe directly")
    p_launch.add_argument("--debug", action="store_true", help="Debug build")
    p_launch.add_argument("--build", action="store_true", help="Build Flutter app before launch")
    p_launch.add_argument("--log-level", type=str, default=None,
                          help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")
    p_launch.add_argument("--test-script", type=str, default=None,
                          help="Path to CSV test script (passed to app via --test-script)")

    # --- demo ---
    p_demo = sub.add_parser("demo", help="Run native Python demo")
    p_demo.add_argument("--debug", action="store_true", help="Use debug build")
    p_demo.add_argument("--build", action="store_true", help="Build native standalone before demo")
    p_demo.add_argument("--log-level", type=str, default=None,
                        help="Log level, e.g. 'debug' or 'trace'")
    p_demo.add_argument("videos", nargs="*", default=[],
                        help="Video file paths (optional, supports multiple)")

    # --- test ---
    p_test = sub.add_parser("test", help="Build and run native standalone tests")
    p_test.add_argument("--debug", action="store_true", help="Debug build")

    # --- vtm ---
    p_vtm = sub.add_parser("vtm", help="VTM DecoderApp: build & H.266 analysis")
    p_vtm.add_argument("vtm_action", choices=["build", "analyze"],
                       help="'build' to compile DecoderApp, 'analyze' to generate .vbs2 stats")
    p_vtm.add_argument("video", nargs="?", default=None,
                       help="Video file path (required for 'analyze')")

    if len(sys.argv) == 1:
        parser.print_help()
        return

    args = parser.parse_args()

    {
        "build": cmd_build,
        "run": cmd_run,
        "launch": cmd_launch,
        "demo": cmd_demo,
        "test": cmd_test,
        "vtm": cmd_vtm,
    }[args.command](args)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted.")
