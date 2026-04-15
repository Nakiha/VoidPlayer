"""VoidPlayer unified dev script — build, run, test from one entry point.

Usage examples:
  python dev.py build                # Build native + Flutter (release)
  python dev.py build --debug        # Build native + Flutter (debug)
  python dev.py build --native       # Build native module only
  python dev.py build --flutter      # Build Flutter only
  python dev.py run                  # Run Flutter app via flutter run (release)
  python dev.py run --debug          # Run Flutter app (debug, hot reload)
  python dev.py launch               # Build + launch exe directly (no resident)
  python dev.py launch --debug       # Build + launch debug exe
  python dev.py demo [video_path]    # Run native demo
  python dev.py demo --debug [path]  # Run native demo (debug build)
  python dev.py test                 # Build + test native module
  python dev.py vtm build            # Build VTM DecoderApp
  python dev.py vtm analyze video.mp4  # Generate .vbs1 binary stats for a video
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent
NATIVE_DIR = ROOT / "windows" / "native"
NATIVE_BUILD_PY = NATIVE_DIR / "build.py"
NATIVE_BUILD_DIR = NATIVE_DIR / "build-msvc"
DEMO_SCRIPT = NATIVE_DIR / "demo" / "demo_video_renderer.py"

VTM_DIR = ROOT / "tools" / "vtm"
VTM_BUILD_DIR = VTM_DIR / "build"
VTM_DECODER = VTM_DIR / "bin" / "mgwmake" / "gcc-mingw-14.2" / "x86_64" / "release" / "DecoderApp.exe"

# MSYS2 paths for VTM build (MinGW GCC toolchain)
MSYS2_BASH = r"C:\msys64\usr\bin\bash.exe"
UCRT64_BIN = r"/ucrt64/bin"


def header(title: str):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print('='*60)


def run(cmd, **kwargs):
    cmd_str = ' '.join(str(c) for c in cmd)
    print(f"> {cmd_str}")
    # Use shell=True only for simple commands, not when invoking bash directly
    use_shell = kwargs.pop("use_shell", True)
    subprocess.check_call(cmd, shell=use_shell, **kwargs)


# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

def cmd_build(args):
    """Build native module and/or Flutter app."""
    if args.native and args.flutter:
        print("ERROR: --native and --flutter are mutually exclusive")
        sys.exit(1)

    build_type = "Debug" if args.debug else "Release"

    # --- Native ---
    if not args.flutter:
        header(f"Build native module ({build_type})")
        build_cmd = [sys.executable, str(NATIVE_BUILD_PY), "--build-only"]
        if args.debug:
            build_cmd.append("--debug")
        run(build_cmd, cwd=str(NATIVE_DIR))

        # Tests are non-blocking — warn on failure, don't abort
        if not args.no_test:
            header(f"Test native module ({build_type})")
            test_cmd = [sys.executable, str(NATIVE_BUILD_PY), "--test-only"]
            if args.debug:
                test_cmd.append("--debug")
            try:
                run(test_cmd, cwd=str(NATIVE_DIR))
            except subprocess.CalledProcessError:
                print("\nWARNING: Native tests failed (continuing with Flutter build)")

    # --- Flutter ---
    if not args.native:
        header(f"Build Flutter ({build_type})")
        flutter_cmd = ["flutter", "build", "windows"]
        if args.debug:
            flutter_cmd.append("--debug")
        run(flutter_cmd, cwd=str(ROOT))

    print("\nBuild done.")


# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------

def cmd_run(args):
    """Run the Flutter application."""
    flutter_args = ["flutter", "run", "-d", "windows"]
    if args.debug:
        flutter_args.append("--debug")
    else:
        flutter_args.append("--release")

    # Forward --log-level to Flutter app
    if args.log_level:
        flutter_args.append("--")
        flutter_args.append(f"--log-level={args.log_level}")

    header(f"Run Flutter ({'debug' if args.debug else 'release'})")
    run(flutter_args, cwd=str(ROOT))


# ---------------------------------------------------------------------------
# launch — build then run exe directly (no flutter run resident)
# ---------------------------------------------------------------------------

def cmd_launch(args):
    """Build Flutter then launch the exe directly."""
    # Ensure cmd_build has the attributes it expects
    args.native = False
    args.flutter = False
    cmd_build(args)

    build_type = "Debug" if args.debug else "Release"
    exe = ROOT / "build" / "windows" / "x64" / "runner" / build_type / "void_player.exe"
    if not exe.exists():
        print(f"ERROR: exe not found: {exe}")
        sys.exit(1)

    cmd = [str(exe)]
    if args.log_level:
        cmd.append(f"--log-level={args.log_level}")
    if args.test_script:
        cmd.extend(["--test-script", str(args.test_script)])

    header(f"Launch {exe}")
    subprocess.call(cmd)  # non-checking: user may close window with non-zero exit


# ---------------------------------------------------------------------------
# demo
# ---------------------------------------------------------------------------

def cmd_demo(args):
    """Run the native Python demo (PySide6 + video_renderer_native)."""
    build_type = "Debug" if args.debug else "Release"

    # Ensure native module is built
    native_lib = NATIVE_BUILD_DIR / build_type / "video_renderer_native.pyd"
    if not native_lib.exists():
        print(f"Native module not found: {native_lib}")
        print("Building native module first...")
        build_cmd = [sys.executable, str(NATIVE_BUILD_PY)]
        if args.debug:
            build_cmd.append("--debug")
        build_cmd.append("--build-only")
        run(build_cmd, cwd=str(NATIVE_DIR))

    demo_cmd = [sys.executable, str(DEMO_SCRIPT)]
    demo_cmd.extend(str(v) for v in args.videos)

    # Pass log level as environment variable SPDLOG_LEVEL
    if args.log_level:
        import os
        env = os.environ.copy()
        env["SPDLOG_LEVEL"] = args.log_level
    else:
        env = None

    header(f"Run native demo ({build_type})")
    run(demo_cmd, cwd=str(NATIVE_DIR), env=env)


# ---------------------------------------------------------------------------
# test
# ---------------------------------------------------------------------------

def cmd_test(args):
    """Build and run native tests."""
    build_type = "Debug" if args.debug else "Release"

    header(f"Build + test native ({build_type})")
    test_cmd = [sys.executable, str(NATIVE_BUILD_PY)]
    if args.debug:
        test_cmd.append("--debug")
    run(test_cmd, cwd=str(NATIVE_DIR))


# ---------------------------------------------------------------------------
# vtm — VTM DecoderApp build & analyze
# ---------------------------------------------------------------------------

def _ensure_submodule():
    """Ensure tools/vtm submodule is initialized and on voidplayer-patches."""
    if not (VTM_DIR / ".git").exists():
        print("VTM submodule not initialized. Running git submodule update...")
        run(["git", "submodule", "update", "--init", "--remote", "tools/vtm"])
    # Verify branch
    result = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        capture_output=True, text=True, cwd=str(VTM_DIR)
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
    ])
    return raw_path


def cmd_vtm(args):
    """Build VTM DecoderApp or generate binary stats."""
    _ensure_submodule()

    if args.vtm_action == "build":
        cmd_vtm_build(args)
    elif args.vtm_action == "analyze":
        if not args.video:
            print("ERROR: 'vtm analyze' requires a video file path")
            sys.exit(1)
        cmd_vtm_analyze(args)
    else:
        print(f"Unknown vtm action: {args.vtm_action}")
        sys.exit(1)


def cmd_vtm_build(_args):
    """Build VTM DecoderApp with DTrace + VBS1 support."""
    VTM_BUILD_DIR.mkdir(parents=True, exist_ok=True)

    build_type = "Release"  # VTM always release (debug is very slow)

    header(f"Configure VTM ({build_type})")
    cmake_cmd = [
        "cmake", str(VTM_DIR),
        "-G", "MinGW Makefiles",
        f"-DCMAKE_BUILD_TYPE={build_type}",
    ]
    run(cmake_cmd, cwd=str(VTM_BUILD_DIR))

    header(f"Build VTM DecoderApp ({build_type})")
    # Run via MSYS2 bash to get proper MinGW GCC environment
    nproc = os.cpu_count() or 4
    # Convert Windows path to MSYS2 POSIX: D:/Code -> /d/Code
    build_posix = VTM_BUILD_DIR.as_posix()
    build_msys = "/" + build_posix[0].lower() + build_posix[2:]
    make_cmd = (
        f'export PATH={UCRT64_BIN}:$PATH && '
        f'cd {build_msys} && '
        f'mingw32-make DecoderApp -j{nproc}'
    )
    run([MSYS2_BASH, "-lc", make_cmd], use_shell=False)

    if VTM_DECODER.exists():
        size_mb = VTM_DECODER.stat().st_size / (1024 * 1024)
        print(f"\n  DecoderApp built: {VTM_DECODER} ({size_mb:.1f} MB)")
    else:
        print(f"\n  WARNING: DecoderApp not found at {VTM_DECODER}")


def cmd_vtm_analyze(args):
    """Generate .vbs2 binary stats, .vbi NALU index, and .vbt timestamps for a video file."""
    if not VTM_DECODER.exists():
        print("DecoderApp not found. Building first...")
        cmd_vtm_build(args)

    video_path = Path(args.video).resolve()
    if not video_path.exists():
        print(f"ERROR: video not found: {video_path}")
        sys.exit(1)

    # Output paths
    vbs2_path = video_path.with_suffix(".vbs2")
    vbi_path = video_path.with_suffix(".vbi")
    vbt_path = video_path.with_suffix(".vbt")

    # Extract raw VVC if needed
    raw_path = _extract_raw_vvc(video_path)

    # --- Step 1: VBS2 (VTM DecoderApp) ---
    header(f"Generate VBS2 stats for {video_path.name}")
    print(f"  Output: {vbs2_path}")

    # Convert Windows path to MSYS2 POSIX path: D:/Code/Foo -> /d/Code/Foo
    def to_msys(p: Path) -> str:
        s = p.as_posix()          # e.g. D:/Code/Foo
        return "/" + s[0].lower() + s[2:]  # strip colon: /d/Code/Foo

    decoder_cmd = (
        f'export PATH={UCRT64_BIN}:$PATH && '
        f'export VTM_BINARY_STATS="{to_msys(vbs2_path)}" && '
        f'"{to_msys(VTM_DECODER)}" '
        f'-b "{to_msys(raw_path)}" '
        f'--TraceFile=/dev/null '
        f'--TraceRule="D_BLOCK_STATISTICS_CODED:poc>=0" '
        f'-o /dev/null'
    )
    run([MSYS2_BASH, "-lc", decoder_cmd], use_shell=False)

    if vbs2_path.exists():
        size_mb = vbs2_path.stat().st_size / (1024 * 1024)
        print(f"\n  Done: {vbs2_path} ({size_mb:.1f} MB)")
    else:
        print(f"\n  ERROR: output file not created: {vbs2_path}")
        sys.exit(1)

    # --- Step 2: VBI (NALU index) ---
    header(f"Generate NALU index for {raw_path.name}")
    from tools.vvc_nalu_indexer import index_nalus
    vbi_result = index_nalus(str(raw_path), str(vbi_path), verbose=True)

    # --- Step 3: VBT (timestamps) ---
    header(f"Extract timestamps from {video_path.name}")
    from tools.vvc_timestamp_extractor import extract_timestamps
    vbt_result = extract_timestamps(str(video_path), str(vbt_path), verbose=True)

    print(f"\n  Analysis complete:")
    print(f"    VBS2: {vbs2_path}")
    print(f"    VBI:  {vbi_path}")
    print(f"    VBT:  {vbt_path}")
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="VoidPlayer dev script",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  python dev.py build                Build all (release)
  python dev.py build --debug        Build all (debug)
  python dev.py build --native       Build native module only
  python dev.py run                  Run Flutter app
  python dev.py run --debug          Run Flutter app (debug mode)
  python dev.py run --log-level flutter=DEBUG,native=TRACE
  python dev.py demo                 Run native demo
  python dev.py demo video.mp4       Run native demo with custom video
  python dev.py test                 Build + test native module
  python dev.py vtm build            Build VTM DecoderApp (MinGW)
  python dev.py vtm analyze video.mp4  Generate .vbs1 binary stats
""",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    # --- build ---
    p_build = sub.add_parser("build", help="Build native module and/or Flutter app")
    p_build.add_argument("--debug", action="store_true", help="Debug build")
    p_build.add_argument("--native", action="store_true", help="Build native module only")
    p_build.add_argument("--flutter", action="store_true", help="Build Flutter only")
    p_build.add_argument("--no-test", action="store_true", help="Skip native tests")

    # --- run ---
    p_run = sub.add_parser("run", help="Run Flutter app")
    p_run.add_argument("--debug", action="store_true", help="Debug mode (hot reload)")
    p_run.add_argument("--log-level", type=str, default=None,
                       help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")

    # --- demo ---
    p_demo = sub.add_parser("demo", help="Run native Python demo")
    p_demo.add_argument("--debug", action="store_true", help="Use debug build")
    p_demo.add_argument("--log-level", type=str, default=None,
                        help="Log level, e.g. 'debug' or 'trace'")
    p_demo.add_argument("videos", nargs="*", default=[],
                        help="Video file paths (optional, supports multiple)")

    # --- test ---
    p_test = sub.add_parser("test", help="Build and run native tests")
    p_test.add_argument("--debug", action="store_true", help="Debug build")

    # --- launch ---
    p_launch = sub.add_parser("launch", help="Build then run exe directly")
    p_launch.add_argument("--debug", action="store_true", help="Debug build")
    p_launch.add_argument("--no-test", action="store_true", help="Skip native tests")
    p_launch.add_argument("--log-level", type=str, default=None,
                           help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")
    p_launch.add_argument("--test-script", type=str, default=None,
                           help="Path to CSV test script (passed to app via --test-script)")

    # --- vtm ---
    p_vtm = sub.add_parser("vtm", help="VTM DecoderApp: build & H.266 analysis")
    p_vtm.add_argument("vtm_action", choices=["build", "analyze"],
                       help="'build' to compile DecoderApp, 'analyze' to generate .vbs1 stats")
    p_vtm.add_argument("video", nargs="?", default=None,
                       help="Video file path (required for 'analyze')")

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
