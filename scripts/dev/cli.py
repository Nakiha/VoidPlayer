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
from .analysis_resize_stress import cmd_analysis_resize_stress
from .package import cmd_package
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
  python dev.py test --flutter-only
  python dev.py test --native-only
  python dev.py package
  python dev.py package --installer
  python dev.py ui-test ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h265.csv
  python dev.py analysis-resize-stress
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

    p_test = sub.add_parser(
        "test",
        help="Run Flutter unit tests and native standalone tests",
    )
    p_test.add_argument("--debug", action="store_true", help="Debug build")
    p_test.add_argument("--flutter-only", action="store_true",
                        help="Run only Flutter unit tests")
    p_test.add_argument("--native-only", action="store_true",
                        help="Run only native standalone tests")

    p_package = sub.add_parser("package", help="Build and stage clean Windows installer input")
    p_package.add_argument("--debug", action="store_true", help=argparse.SUPPRESS)
    p_package.add_argument("--no-build", action="store_true",
                           help="Skip Flutter build and stage the existing clean Release output")
    p_package.add_argument("--installer", action="store_true",
                           help="Compile the Inno Setup installer after staging")
    p_package.add_argument("--iscc", type=str, default=None,
                           help="Path to ISCC.exe (defaults to PATH/common Inno Setup locations)")

    p_ui_test = sub.add_parser("ui-test", help="Launch the app with a CSV UI test script")
    p_ui_test.add_argument("scripts", nargs="+", help="Path(s) to CSV test script(s)")
    p_ui_test.add_argument("--debug", action="store_true", help="Debug build")
    p_ui_test.add_argument("--build", action="store_true", help="Build Flutter app before launch")
    p_ui_test.add_argument("--log-level", type=str, default=None,
                           help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")
    p_ui_test.add_argument("--visible", action="store_true",
                           help="Show and focus test windows instead of using silent no-activate mode")

    p_analysis_resize = sub.add_parser(
        "analysis-resize-stress",
        help="Launch standalone analysis and stress-resize its window",
    )
    p_analysis_resize.add_argument("--debug", action="store_true", help="Use Debug build")
    p_analysis_resize.add_argument("--build", action="store_true", help="Build Flutter app before test")
    p_analysis_resize.add_argument("--hash", type=str, default=None,
                                   help="Analysis cache hash to open (default: pick a cached entry)")
    p_analysis_resize.add_argument("--rounds", type=int, default=5,
                                   help="Number of resize rounds")
    p_analysis_resize.add_argument("--visible", action="store_true",
                                   help="Show and focus the analysis window instead of silent mode")

    p_vtm = sub.add_parser("vtm", help="VTM DecoderApp: build & H.266 analysis")
    p_vtm.add_argument("vtm_action", choices=["build", "analyze"],
                       help="'build' to compile DecoderApp, 'analyze' to generate .vbs2/.vvc stats")
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
        "package": cmd_package,
        "ui-test": cmd_ui_test,
        "analysis-resize-stress": cmd_analysis_resize_stress,
        "vtm": cmd_vtm,
    }[args.command](args)
