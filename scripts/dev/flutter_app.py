"""Flutter app build, launch, demo, and UI test commands."""

import os
import subprocess
import sys
from pathlib import Path

from .native import native_build
from .paths import DEMO_SCRIPT, NATIVE_BUILD_DIR, NATIVE_DIR, ROOT, app_exe_path
from .process import header, run
from .ui_lock import gui_test_lock


def flutter_build(debug: bool) -> None:
    """Build Flutter Windows app."""
    build_type = "Debug" if debug else "Release"
    header(f"Build Flutter ({build_type})")

    cmd = ["flutter", "build", "windows"]
    cmd.append("--debug" if debug else "--release")

    run(cmd, cwd=str(ROOT))


def cmd_build(args) -> None:
    """Build native standalone module and/or Flutter app."""
    if args.native and args.flutter:
        print("ERROR: --native and --flutter are mutually exclusive")
        sys.exit(1)

    if not args.flutter:
        native_build(args.debug, test=not args.no_test)

    if not args.native:
        flutter_build(args.debug)

    print("\nBuild done.")


def cmd_run(args) -> None:
    """Run the Flutter application via flutter run."""
    flutter_args = ["flutter", "run", "-d", "windows"]
    flutter_args.append("--debug" if args.debug else "--release")

    if args.log_level:
        flutter_args.extend(["--", f"--log-level={args.log_level}"])

    header(f"Run Flutter ({'debug' if args.debug else 'release'})")
    run(flutter_args, cwd=str(ROOT))


def cmd_launch(args) -> None:
    """Launch exe directly; build Flutter first only if requested or missing."""
    if args.test_script:
        try:
            with gui_test_lock("launch --test-script"):
                _cmd_launch(args)
        except RuntimeError as exc:
            print(f"Launch test failed: {exc}")
            sys.exit(1)
        return

    _cmd_launch(args)


def _cmd_launch(args) -> None:
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


def cmd_demo(args) -> None:
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


def cmd_test(args) -> None:
    """Build and run native standalone tests."""
    native_build(args.debug, test=True)


def cmd_ui_test(args) -> None:
    """Launch the app with a CSV script and use process exit code as result."""
    try:
        with gui_test_lock("ui-test"):
            _cmd_ui_test(args)
    except RuntimeError as exc:
        print(f"UI test failed: {exc}")
        sys.exit(1)


def _cmd_ui_test(args) -> None:
    script_path = Path(args.script).resolve()
    if not script_path.exists():
        print(f"ERROR: test script not found: {script_path}")
        sys.exit(1)

    exe = app_exe_path(args.debug)

    if args.build or not exe.exists():
        flutter_build(args.debug)

    if not exe.exists():
        print(f"ERROR: exe not found: {exe}")
        sys.exit(1)

    cmd = [str(exe), "--test-script", str(script_path)]
    if not args.visible:
        cmd.append("--silent-ui-test")
    if args.log_level:
        cmd.append(f"--log-level={args.log_level}")

    header(f"UI test {script_path.name}")
    result = subprocess.call(cmd, cwd=str(ROOT))
    if result != 0:
        print(f"\nUI test failed with exit code {result}")
        sys.exit(result)

    print("\nUI test passed.")
