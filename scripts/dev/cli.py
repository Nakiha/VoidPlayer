"""Command-line parser for VoidPlayer development commands."""

import argparse
import sys

from .flutter_app import (
    cmd_build,
    cmd_demo,
    cmd_launch,
    cmd_run,
    cmd_test,
    cmd_ui_test,
)
from .vtm import cmd_vtm


def build_parser() -> argparse.ArgumentParser:
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
  python dev.py ui-test ui_tests/smoke_basic.csv
  python dev.py vtm build
  python dev.py vtm analyze video.mp4
""",
    )
    sub = parser.add_subparsers(dest="command")

    p_build = sub.add_parser("build", help="Build native standalone and/or Flutter app")
    p_build.add_argument("--debug", action="store_true", help="Debug build")
    p_build.add_argument("--native", action="store_true", help="Build native standalone only")
    p_build.add_argument("--flutter", action="store_true", help="Build Flutter app only")
    p_build.add_argument("--no-test", action="store_true", help="Skip native standalone tests")

    p_run = sub.add_parser("run", help="Run Flutter app via flutter run")
    p_run.add_argument("--debug", action="store_true", help="Debug mode (hot reload)")
    p_run.add_argument("--log-level", type=str, default=None,
                       help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")

    p_launch = sub.add_parser("launch", help="Launch exe directly")
    p_launch.add_argument("--debug", action="store_true", help="Debug build")
    p_launch.add_argument("--build", action="store_true", help="Build Flutter app before launch")
    p_launch.add_argument("--log-level", type=str, default=None,
                          help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")
    p_launch.add_argument("--test-script", type=str, default=None,
                          help="Path to CSV test script (passed to app via --test-script)")

    p_demo = sub.add_parser("demo", help="Run native Python demo")
    p_demo.add_argument("--debug", action="store_true", help="Use debug build")
    p_demo.add_argument("--build", action="store_true", help="Build native standalone before demo")
    p_demo.add_argument("--log-level", type=str, default=None,
                        help="Log level, e.g. 'debug' or 'trace'")
    p_demo.add_argument("videos", nargs="*", default=[],
                        help="Video file paths (optional, supports multiple)")

    p_test = sub.add_parser("test", help="Build and run native standalone tests")
    p_test.add_argument("--debug", action="store_true", help="Debug build")

    p_ui_test = sub.add_parser("ui-test", help="Launch the app with a CSV UI test script")
    p_ui_test.add_argument("script", help="Path to CSV test script")
    p_ui_test.add_argument("--debug", action="store_true", help="Debug build")
    p_ui_test.add_argument("--build", action="store_true", help="Build Flutter app before launch")
    p_ui_test.add_argument("--log-level", type=str, default=None,
                           help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")
    p_ui_test.add_argument("--visible", action="store_true",
                           help="Show and focus test windows instead of using silent no-activate mode")

    p_vtm = sub.add_parser("vtm", help="VTM DecoderApp: build & H.266 analysis")
    p_vtm.add_argument("vtm_action", choices=["build", "analyze"],
                       help="'build' to compile DecoderApp, 'analyze' to generate .vbs2 stats")
    p_vtm.add_argument("video", nargs="?", default=None,
                       help="Video file path (required for 'analyze')")

    return parser


def main() -> None:
    parser = build_parser()

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
        "ui-test": cmd_ui_test,
        "vtm": cmd_vtm,
    }[args.command](args)
