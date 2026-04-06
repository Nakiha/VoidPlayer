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
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent
NATIVE_DIR = ROOT / "windows" / "native"
NATIVE_BUILD_PY = NATIVE_DIR / "build.py"
NATIVE_BUILD_DIR = NATIVE_DIR / "build-msvc"
DEMO_SCRIPT = NATIVE_DIR / "demo" / "demo_video_renderer.py"


def header(title: str):
    print(f"\n{'='*60}")
    print(f"  {title}")
    print('='*60)


def run(cmd, **kwargs):
    cmd_str = ' '.join(str(c) for c in cmd)
    print(f"> {cmd_str}")
    subprocess.check_call(cmd, shell=True, **kwargs)


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
# CLI
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

    args = parser.parse_args()

    {
        "build": cmd_build,
        "run": cmd_run,
        "launch": cmd_launch,
        "demo": cmd_demo,
        "test": cmd_test,
    }[args.command](args)


if __name__ == "__main__":
    main()
