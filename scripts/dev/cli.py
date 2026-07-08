"""Command-line parser for VoidPlayer development commands."""

import argparse
import sys

from .config import load_dev_config
from .flutter_app import (
    cmd_build,
    cmd_demo,
    cmd_launch,
    cmd_mac_ui_test,
    cmd_run,
    cmd_test,
    cmd_ui_test,
)
from .gate import cmd_gate
from .package import cmd_package
from .flutter_toolchain import (
    bootstrap_flutter_toolchain,
    print_flutter_toolchain_doctor,
    print_flutter_toolchain_lock,
)


def _require_windows_command(command: str) -> None:
    if sys.platform == "win32":
        return
    print(f"ERROR: dev.py {command} is only supported on Windows.")
    sys.exit(1)


def _cmd_agent(args) -> None:
    from .agent_client import cmd_agent as impl

    impl(args)


def _cmd_agent_smoke(args) -> None:
    from .agent_client import cmd_agent_smoke as impl

    impl(args)


def cmd_analysis_resize_stress(args) -> None:
    _require_windows_command("analysis-resize-stress")
    from .analysis_resize_stress import cmd_analysis_resize_stress as impl

    impl(args)


def cmd_analysis_benchmark(args) -> None:
    from .analysis_benchmark import cmd_analysis_benchmark as impl

    impl(args)


def cmd_analysis_overlay_benchmark(args) -> None:
    from .analysis_overlay_benchmark import cmd_analysis_overlay_benchmark as impl

    impl(args)


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
  python dev.py test --native-only --github
  python dev.py gate pr-fast
  python dev.py gate macos-ui-smoke
  python dev.py gate macos-release-readiness
  python dev.py gate flutter-fork-protection
  python dev.py gate macos-platform-protection
  python dev.py package
  python dev.py package --installer
  python dev.py package --installer --macos-sign-identity "Developer ID Application: Team" --macos-notarize --macos-notary-profile PROFILE
  python dev.py toolchain doctor
  python dev.py toolchain bootstrap-flutter
  python dev.py ui-test ui_tests/smoke/basic.csv ui_tests/analysis/spawn_h265.csv
  python dev.py mac-ui-test ui_tests/macos/native_facade_smoke.csv
  python dev.py analysis-resize-stress
  python dev.py analysis-benchmark --build
  python dev.py analysis-overlay-benchmark --build
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

    p_launch = sub.add_parser("launch", help="Launch the built desktop app directly")
    p_launch.add_argument("--debug", action="store_true", help="Debug build")
    p_launch.add_argument("--build", action="store_true", help="Build Flutter app before launch")
    p_launch.add_argument("--log-level", type=str, default=None,
                          help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")
    p_launch.add_argument("--test-script", type=str, default=None,
                          help="Path to CSV test script (passed to app via --test-script)")

    p_demo = sub.add_parser("demo", help="Run native Python demo (Windows)")
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
    p_test.add_argument("--github", action="store_true",
                        help="Run the lightweight native test set used by GitHub Actions")

    p_gate = sub.add_parser("gate", help="Run a named validation gate profile")
    p_gate.add_argument(
        "profile",
        choices=[
            "pr-fast",
            "macos-native-fast",
            "macos-native-werror",
            "macos-native-sanitizers",
            "macos-ui-smoke",
            "macos-ui-nightly",
            "macos-release-readiness",
            "repo-hygiene",
            "flutter-fork-protection",
            "macos-platform-protection",
            "windows-fork-protection",
            "windows-preservation",
            "release-candidate",
        ],
        help="Gate profile to run",
    )

    p_package = sub.add_parser("package", help="Build and stage clean platform package input")
    p_package.add_argument("--debug", action="store_true", help=argparse.SUPPRESS)
    p_package.add_argument("--no-build", action="store_true",
                           help="Skip Flutter build and stage the existing clean Release output")
    p_package.add_argument("--installer", action="store_true",
                           help="Create the platform installer after staging")
    p_package.add_argument("--iscc", type=str, default=None,
                           help="Path to ISCC.exe (defaults to PATH/common Inno Setup locations)")
    p_package.add_argument("--macos-sign-identity", type=str, default=None,
                           help="macOS Developer ID Application identity; defaults to VOIDPLAYER_MACOS_SIGN_IDENTITY")
    p_package.add_argument("--macos-notarize", action="store_true",
                           help="Submit and staple the macOS DMG with xcrun notarytool")
    p_package.add_argument("--macos-notary-profile", type=str, default=None,
                           help="notarytool keychain profile; defaults to VOIDPLAYER_MACOS_NOTARY_PROFILE")

    p_toolchain = sub.add_parser("toolchain", help="Manage pinned external toolchains")
    p_toolchain.add_argument(
        "action",
        choices=["doctor", "bootstrap-flutter", "print-flutter-lock"],
        help="Toolchain action to run",
    )

    p_ui_test = sub.add_parser("ui-test", help="Launch the Windows app with CSV UI test scripts")
    p_ui_test.add_argument("scripts", nargs="+", help="Path(s) to CSV test script(s)")
    p_ui_test.add_argument("--debug", action="store_true", help="Debug build")
    p_ui_test.add_argument("--build", action="store_true", help="Build Flutter app before launch")
    p_ui_test.add_argument("--log-level", type=str, default=None,
                           help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")
    p_ui_test.add_argument("--visible", action="store_true",
                           help="Show and focus test windows instead of using silent no-activate mode")

    p_mac_ui_test = sub.add_parser(
        "mac-ui-test",
        help="Launch the macOS app with CSV UI test scripts copied into its sandbox",
    )
    p_mac_ui_test.add_argument("scripts", nargs="+", help="Path(s) to CSV test script(s)")
    p_mac_ui_test_build = p_mac_ui_test.add_mutually_exclusive_group()
    p_mac_ui_test_build.add_argument("--debug", dest="debug", action="store_true",
                                     help="Use Debug build (default)")
    p_mac_ui_test_build.add_argument("--release", dest="debug", action="store_false",
                                     help="Use Release build")
    p_mac_ui_test.set_defaults(debug=True)
    p_mac_ui_test.add_argument("--build", action="store_true", help="Build Flutter app before launch")
    p_mac_ui_test.add_argument("--log-level", type=str, default=None,
                               help="Log level, e.g. 'flutter=DEBUG,native=TRACE'")
    p_mac_ui_test.add_argument("--visible", action="store_true",
                               help="Show and focus test windows instead of using silent no-activate mode")

    p_agent = sub.add_parser(
        "agent",
        help="Run one agent protocol verb against a running VoidPlayer "
        "instance (see lib/docs/AGENT_PROTOCOL.md)",
    )
    p_agent.add_argument(
        "verb",
        choices=("session", "marks", "export", "play", "pause", "seek",
                 "set-source-id", "add-media"),
    )
    p_agent.add_argument("--connection-file", required=True,
                         help="Path passed to --agent-connection-file at launch")
    p_agent.add_argument("--timeout", type=float, default=30.0,
                         help="Seconds to wait for the connection file")
    p_agent.add_argument("--path", default=None, help="Output path for export")
    p_agent.add_argument("--pts-us", type=int, default=None,
                         help="Seek target in microseconds")
    p_agent.add_argument("--slot", type=int, default=None,
                         help="Track slot index for set-source-id")
    p_agent.add_argument("--source-id", default=None,
                         help="Source lineage id for set-source-id")

    p_agent_smoke = sub.add_parser(
        "agent-smoke",
        help="End-to-end agent protocol smoke against the real macOS app",
    )
    p_agent_smoke_build = p_agent_smoke.add_mutually_exclusive_group()
    p_agent_smoke_build.add_argument("--debug", dest="debug",
                                     action="store_true",
                                     help="Use Debug build (default)")
    p_agent_smoke_build.add_argument("--release", dest="debug",
                                     action="store_false",
                                     help="Use Release build")
    p_agent_smoke.set_defaults(debug=True)
    p_agent_smoke.add_argument("--build", action="store_true",
                               help="Build Flutter app before launch")
    p_agent_smoke.add_argument("--log-level", type=str, default=None,
                               help="Log level, e.g. 'flutter=DEBUG'")

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

    p_analysis_benchmark = sub.add_parser(
        "analysis-benchmark",
        help="Benchmark full-file VAC2 + VACHUNK generation for bundled samples",
    )
    p_analysis_benchmark.add_argument("--build", action="store_true",
                                      help="Build Flutter release before benchmarking")
    p_analysis_benchmark.add_argument("--cache-root", type=str, default=None,
                                      help="Cache root to use (default: build/analysis-benchmark/cache)")
    p_analysis_benchmark.add_argument("--output-dir", type=str, default=None,
                                      help="Report output directory (default: build/analysis-benchmark)")
    p_analysis_benchmark.add_argument("--keep-cache", action="store_true",
                                      help="Reuse existing benchmark cache root")
    p_analysis_benchmark.add_argument("samples", nargs="*",
                                      help="Optional sample codec/name filters: h264 h265 h266, or hevc/vvc")

    p_overlay_benchmark = sub.add_parser(
        "analysis-overlay-benchmark",
        help="Benchmark native overlay heatmap rasterization",
    )
    p_overlay_benchmark.add_argument("--build", action="store_true",
                                     help="Build Flutter release before benchmarking")
    p_overlay_benchmark.add_argument("--cache-root", type=str, default=None,
                                     help="Cache root to use (default: build/analysis-overlay-benchmark/cache)")
    p_overlay_benchmark.add_argument("--output-dir", type=str, default=None,
                                     help="Report output directory (default: build/analysis-overlay-benchmark)")
    p_overlay_benchmark.add_argument("--keep-cache", action="store_true",
                                     help="Reuse existing benchmark cache root")
    p_overlay_benchmark.add_argument("--video", type=str, default=None,
                                     help="Video path (default: resources/video/h266_10s_1920x1080.mp4)")
    p_overlay_benchmark.add_argument("--codec", type=str, default="vvc",
                                     choices=["h264", "hevc", "vvc"],
                                     help="Codec for --video (default: vvc)")
    p_overlay_benchmark.add_argument("--frame", type=int, default=0,
                                     help="Frame index to benchmark")
    p_overlay_benchmark.add_argument("--width", type=int, default=1920,
                                     help="Raster surface width")
    p_overlay_benchmark.add_argument("--height", type=int, default=1080,
                                     help="Raster surface height")
    p_overlay_benchmark.add_argument("--iterations", type=int, default=120,
                                     help="Raster iterations")
    p_overlay_benchmark.add_argument("--mode", type=str, default="bitrate",
                                     choices=["bitrate", "qp"],
                                     help="Heatmap mode")
    p_overlay_benchmark.add_argument("--with-grid", action="store_true",
                                     help="Also raster the CU/MB boundary mask")
    p_overlay_benchmark.add_argument("--skip-gpu", action="store_true",
                                     help="Skip GPU timestamp benchmark")

    return parser


def main() -> None:
    load_dev_config()
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
        "gate": cmd_gate,
        "package": cmd_package,
        "toolchain": cmd_toolchain,
        "ui-test": cmd_ui_test,
        "mac-ui-test": cmd_mac_ui_test,
        "agent": _cmd_agent,
        "agent-smoke": _cmd_agent_smoke,
        "analysis-resize-stress": cmd_analysis_resize_stress,
        "analysis-benchmark": cmd_analysis_benchmark,
        "analysis-overlay-benchmark": cmd_analysis_overlay_benchmark,
    }[args.command](args)


def cmd_toolchain(args) -> None:
    if args.action == "doctor":
        print_flutter_toolchain_doctor()
        return
    if args.action == "bootstrap-flutter":
        bootstrap_flutter_toolchain()
        return
    if args.action == "print-flutter-lock":
        print_flutter_toolchain_lock()
        return
    print(f"ERROR: unknown toolchain action: {args.action}")
    sys.exit(1)
