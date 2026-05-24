"""Flutter app build, launch, demo, and UI test commands."""

import csv
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from .native import ensure_ffmpeg_analyzer_tool, native_build, native_build_macos
from .paths import (
    DEMO_SCRIPT,
    NATIVE_BUILD_DIR,
    NATIVE_DIR,
    ROOT,
    app_exe_path,
    macos_app_bundle_path,
    macos_app_exe_path,
)
from .process import header, run
from .ui_lock import gui_test_lock


def _is_macos() -> bool:
    return sys.platform == "darwin"


def _is_windows() -> bool:
    return sys.platform == "win32"


def _unsupported_on_current_platform(command: str, replacement: str | None = None) -> None:
    print(f"ERROR: dev.py {command} is only supported on Windows.")
    if replacement:
        print(f"Use: {replacement}")
    sys.exit(1)


def flutter_build(debug: bool) -> None:
    """Build Flutter Windows app."""
    build_type = "Debug" if debug else "Release"
    ensure_ffmpeg_analyzer_tool()

    header(f"Build Flutter ({build_type})")

    cmd = ["flutter", "build", "windows"]
    cmd.append("--debug" if debug else "--release")

    run(cmd, cwd=str(ROOT))


def flutter_build_macos(debug: bool) -> None:
    """Build Flutter macOS app."""
    build_type = "Debug" if debug else "Release"

    header(f"Build Flutter macOS ({build_type})")

    cmd = ["flutter", "build", "macos"]
    cmd.append("--debug" if debug else "--release")

    run(cmd, cwd=str(ROOT))


def flutter_unit_test() -> None:
    """Run Flutter/Dart unit tests that do not launch the Windows app."""
    header("Analyze Flutter")
    run(["flutter", "analyze"], cwd=str(ROOT))

    header("Test Flutter unit")
    run(["flutter", "test", "test/unit"], cwd=str(ROOT))


def _is_macos_launch_source(path: Path) -> bool:
    rel = path.relative_to(ROOT)
    parts = rel.parts
    if not parts:
        return False
    if parts[0] in {".dart_tool", ".git", "build"}:
        return False
    if parts[0] == "native" and (
        len(parts) > 1 and (parts[1].startswith("build") or parts[1] == "analysis")
    ):
        return False
    if parts[0] == "macos" and len(parts) > 1 and parts[1] in {"Pods", "Flutter"}:
        return False
    if parts[0] in {"lib", "macos", "native", "third_party"}:
        return path.suffix.lower() in {
            ".c",
            ".cc",
            ".cmake",
            ".cpp",
            ".dart",
            ".h",
            ".hpp",
            ".m",
            ".mm",
            ".metal",
            ".podspec",
            ".swift",
            ".txt",
            ".yaml",
        }
    return rel.as_posix() in {
        "CMakeLists.txt",
        "pubspec.yaml",
        "pubspec.lock",
    }


def _macos_app_stale(exe: Path) -> bool:
    if not exe.exists():
        return True
    exe_mtime = exe.stat().st_mtime
    for name in ("pubspec.yaml", "pubspec.lock"):
        path = ROOT / name
        try:
            if path.exists() and path.stat().st_mtime > exe_mtime:
                return True
        except OSError:
            continue

    for dirpath, dirnames, filenames in os.walk(ROOT):
        path = Path(dirpath)
        rel_parts = path.relative_to(ROOT).parts
        if not rel_parts:
            dirnames[:] = [
                name for name in dirnames
                if name in {"lib", "macos", "native", "third_party"}
            ]
        elif rel_parts == ("macos",):
            dirnames[:] = [name for name in dirnames if name not in {"Flutter", "Pods"}]
        elif rel_parts == ("native",):
            dirnames[:] = [
                name for name in dirnames
                if not name.startswith("build") and name != "analysis"
            ]
        elif rel_parts == ("third_party",):
            dirnames[:] = [name for name in dirnames if name == "ffmpeg"]

        for filename in filenames:
            source = path / filename
            try:
                if _is_macos_launch_source(source) and source.stat().st_mtime > exe_mtime:
                    return True
            except (OSError, ValueError):
                continue
    return False


def cmd_build(args) -> None:
    """Build native standalone module and/or Flutter app."""
    if args.native and args.flutter:
        print("ERROR: --native and --flutter are mutually exclusive")
        sys.exit(1)

    if not args.flutter:
        if _is_macos():
            native_build_macos(args.debug, test=not args.no_test)
        else:
            native_build(args.debug, test=not args.no_test)

    if not args.native:
        if _is_macos():
            flutter_build_macos(args.debug)
        else:
            flutter_build(args.debug)

    print("\nBuild done.")


def cmd_run(args) -> None:
    """Run the Flutter application via flutter run."""
    device = "macos" if _is_macos() else "windows"
    flutter_args = ["flutter", "run", "-d", device]
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
    if _is_macos():
        _cmd_launch_macos(args)
        return

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


def _cmd_launch_macos(args) -> None:
    app_bundle = macos_app_bundle_path(args.debug)
    exe = macos_app_exe_path(args.debug)

    if args.build or not app_bundle.exists() or _macos_app_stale(exe):
        flutter_build_macos(args.debug)

    if not app_bundle.exists() or not exe.exists():
        print(f"ERROR: macOS app not found: {app_bundle}")
        sys.exit(1)

    cmd = [str(exe)]
    if args.log_level:
        cmd.append(f"--log-level={args.log_level}")
    if args.test_script:
        script_path = Path(args.test_script).resolve()
        if not script_path.exists():
            print(f"ERROR: test script not found: {script_path}")
            sys.exit(1)
        container_scripts_dir, container_media_dir = _macos_ui_test_container_dirs()
        sandbox_script = container_scripts_dir / script_path.name
        _prepare_macos_sandbox_script(script_path, sandbox_script, container_media_dir)
        cmd.extend(["--test-script", str(sandbox_script)])

    header(f"Launch {exe}")
    subprocess.call(cmd)


def cmd_demo(args) -> None:
    """Run the native Python demo (PySide6 + video_renderer_native)."""
    if not _is_windows():
        _unsupported_on_current_platform("demo")

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
    """Run Flutter unit tests and native standalone tests."""
    if args.flutter_only and args.native_only:
        print("ERROR: --flutter-only and --native-only are mutually exclusive")
        sys.exit(1)

    if not args.native_only:
        flutter_unit_test()

    if not args.flutter_only:
        if _is_macos():
            if args.github:
                print("macOS native test mode ignores --github; running the local CTest suite.")
            native_build_macos(args.debug, test=True)
        else:
            native_build(args.debug, test=True, github=args.github)


def cmd_ui_test(args) -> None:
    """Launch the app with a CSV script and use process exit code as result."""
    if not _is_windows():
        _unsupported_on_current_platform(
            "ui-test",
            "python dev.py mac-ui-test <script.csv>",
        )

    try:
        with gui_test_lock("ui-test"):
            _cmd_ui_test(args)
    except RuntimeError as exc:
        print(f"UI test failed: {exc}")
        sys.exit(1)


def _script_app_args(script_path: Path) -> list[str]:
    """Read app startup arguments declared by CSV test headers."""
    app_args: list[str] = []
    try:
        with script_path.open("r", encoding="utf-8-sig", newline="") as file:
            for row in csv.reader(file):
                if not row:
                    continue
                key = row[0].strip().upper()
                if key not in ("@APP_ARG", "@APP_ARGS"):
                    continue
                app_args.extend(value.strip() for value in row[1:] if value.strip())
    except OSError as exc:
        raise RuntimeError(f"cannot read UI test headers from {script_path}: {exc}") from exc

    return app_args


def _cmd_ui_test(args) -> None:
    script_paths = [Path(script).resolve() for script in args.scripts]
    for script_path in script_paths:
        if not script_path.exists():
            print(f"ERROR: test script not found: {script_path}")
            sys.exit(1)

    exe = app_exe_path(args.debug)

    if args.build or not exe.exists():
        flutter_build(args.debug)

    if not exe.exists():
        print(f"ERROR: exe not found: {exe}")
        sys.exit(1)

    total = len(script_paths)
    for index, script_path in enumerate(script_paths, start=1):
        cmd = [str(exe), "--test-script", str(script_path)]
        cmd.extend(_script_app_args(script_path))
        if not args.visible:
            cmd.append("--silent-ui-test")
        if args.log_level:
            cmd.append(f"--log-level={args.log_level}")

        label = script_path.relative_to(ROOT) if script_path.is_relative_to(ROOT) else script_path
        header(f"UI test {index}/{total} {label}")
        result, ax_tree_error = _run_ui_test_process(cmd)
        if ax_tree_error:
            print(f"\nUI test failed: Flutter AXTree error detected: {label}")
            sys.exit(1)
        if result != 0:
            print(f"\nUI test failed with exit code {result}: {label}")
            sys.exit(result)

    print(f"\nUI test batch passed ({total} script{'s' if total != 1 else ''}).")


def cmd_mac_ui_test(args) -> None:
    """Launch the macOS app with CSV scripts copied into the app sandbox."""
    if sys.platform != "darwin":
        print("ERROR: mac-ui-test is only supported on macOS")
        sys.exit(1)

    try:
        with gui_test_lock("mac-ui-test"):
            _cmd_mac_ui_test(args)
    except RuntimeError as exc:
        print(f"macOS UI test failed: {exc}")
        sys.exit(1)


def _cmd_mac_ui_test(args) -> None:
    script_paths = [Path(script).resolve() for script in args.scripts]
    for script_path in script_paths:
        if not script_path.exists():
            print(f"ERROR: test script not found: {script_path}")
            sys.exit(1)

    app_bundle = macos_app_bundle_path(args.debug)
    exe = macos_app_exe_path(args.debug)

    if args.build or not app_bundle.exists() or not exe.exists():
        flutter_build_macos(args.debug)

    if not app_bundle.exists() or not exe.exists():
        print(f"ERROR: macOS app not found: {app_bundle}")
        sys.exit(1)

    container_scripts_dir, container_media_dir = _macos_ui_test_container_dirs()

    total = len(script_paths)
    for index, script_path in enumerate(script_paths, start=1):
        sandbox_script = (
            container_scripts_dir / f"{index:02d}_{script_path.name}"
        )
        _prepare_macos_sandbox_script(script_path, sandbox_script, container_media_dir)

        app_args = ["--test-script", str(sandbox_script)]
        app_args.extend(_script_app_args(script_path))
        if not args.visible:
            app_args.append("--silent-ui-test")
        if args.log_level:
            app_args.append(f"--log-level={args.log_level}")

        label = (
            script_path.relative_to(ROOT)
            if script_path.is_relative_to(ROOT)
            else script_path
        )
        header(f"macOS UI test {index}/{total} {label}")
        before_crash_reports = _macos_diagnostic_report_paths()
        if args.visible:
            result, ax_tree_error = _run_ui_test_process([str(exe), *app_args])
        else:
            result, ax_tree_error = _run_macos_ui_test_process(
                app_bundle,
                app_args,
            )
        crash_reports = _wait_for_new_macos_crash_reports(before_crash_reports)
        if crash_reports:
            _print_macos_crash_reports(crash_reports)
            if result == 0:
                result = 1
        if ax_tree_error:
            print(f"\nmacOS UI test failed: Flutter AXTree error detected: {label}")
            sys.exit(1)
        if result != 0:
            print(f"\nmacOS UI test failed with exit code {result}: {label}")
            sys.exit(result)

    print(f"\nmacOS UI test batch passed ({total} script{'s' if total != 1 else ''}).")


def _macos_ui_test_container_dirs() -> tuple[Path, Path]:
    container_scripts_dir = (
        Path.home()
        / "Library"
        / "Containers"
        / "dev.nakiha.voidplayer"
        / "Data"
        / "tmp"
        / "voidplayer-ui-tests"
    )
    container_scripts_dir.mkdir(parents=True, exist_ok=True)
    container_media_dir = container_scripts_dir / "media"
    container_media_dir.mkdir(parents=True, exist_ok=True)
    return container_scripts_dir, container_media_dir


def _macos_diagnostic_report_paths() -> set[Path]:
    reports_dir = Path.home() / "Library" / "Logs" / "DiagnosticReports"
    if not reports_dir.exists():
        return set()
    reports: set[Path] = set()
    for pattern in ("VoidPlayer*.ips", "void_player*.ips"):
        reports.update(path for path in reports_dir.glob(pattern) if path.is_file())
    return reports


def _wait_for_new_macos_crash_reports(before: set[Path]) -> list[Path]:
    deadline = time.monotonic() + 3.0
    new_reports: set[Path] = set()
    while time.monotonic() < deadline:
        new_reports = _macos_diagnostic_report_paths() - before
        if new_reports:
            break
        time.sleep(0.1)
    return sorted(new_reports, key=lambda path: path.stat().st_mtime)


def _print_macos_crash_reports(reports: list[Path]) -> None:
    print("\nmacOS crash report generated during UI test:")
    for report in reports:
        print(f"  {report}")
        for line in _macos_crash_report_summary(report):
            print(f"    {line}")


def _macos_crash_report_summary(report: Path) -> list[str]:
    try:
        raw = report.read_text(encoding="utf-8", errors="replace")
        first_line, _, body = raw.partition("\n")
        header_data = json.loads(first_line) if first_line else {}
        data = json.loads(body) if body else {}
    except Exception as exc:
        return [f"summary unavailable: {exc}"]

    lines: list[str] = []
    app_name = header_data.get("app_name") or data.get("procName") or "unknown"
    timestamp = header_data.get("timestamp") or data.get("captureTime") or "unknown time"
    exception = data.get("exception") or {}
    termination = data.get("termination") or {}
    exception_text = exception.get("type") or "unknown exception"
    signal_text = exception.get("signal") or termination.get("indicator")
    if signal_text:
        exception_text = f"{exception_text} / {signal_text}"
    lines.append(f"{app_name} at {timestamp}: {exception_text}")

    faulting_thread = data.get("faultingThread")
    threads = data.get("threads") or []
    if isinstance(faulting_thread, int) and 0 <= faulting_thread < len(threads):
        frames = threads[faulting_thread].get("frames") or []
        symbols: list[str] = []
        for frame in frames[:8]:
            symbol = frame.get("symbol") or frame.get("sourceFile") or "<unknown>"
            source = frame.get("sourceFile")
            line = frame.get("sourceLine")
            if source and line:
                symbol = f"{symbol} ({source}:{line})"
            symbols.append(symbol)
        if symbols:
            lines.append("faulting thread: " + " <- ".join(symbols))
    return lines


def _prepare_macos_sandbox_script(
    script_path: Path,
    sandbox_script: Path,
    container_media_dir: Path,
) -> None:
    """Copy a macOS UI script and make repo media fixtures sandbox-readable."""
    generated_media: dict[str, str] = {}
    with script_path.open("r", encoding="utf-8-sig", newline="") as source:
        with sandbox_script.open("w", encoding="utf-8", newline="") as dest:
            reader = csv.reader(source)
            writer = csv.writer(dest, lineterminator="\n")
            for row in reader:
                if _is_generate_media_row(row):
                    original_path = row[2].strip()
                    sandbox_path = _macos_generated_media_path(
                        original_path,
                        container_media_dir,
                    )
                    generated_media[original_path] = sandbox_path
                    _generate_macos_test_video(row, sandbox_path)
                    continue
                elif _is_add_media_row(row):
                    media_path = row[2].strip()
                    row[2] = generated_media.get(
                        media_path,
                        _macos_sandbox_media_path(media_path, container_media_dir),
                    )
                writer.writerow(row)


def _is_add_media_row(row: list[str]) -> bool:
    return len(row) >= 3 and row[1].strip().upper() == "ADD_MEDIA"


def _is_generate_media_row(row: list[str]) -> bool:
    return (
        len(row) >= 3
        and row[1].strip().upper()
        in {"GENERATE_TEST_VIDEO", "GENERATE_TEST_VIDEO_WITH_AUDIO"}
    )


def _macos_generated_media_path(media_path: str, container_media_dir: Path) -> str:
    path = Path(media_path.strip())
    if path.is_absolute():
        return str(path)
    dest = container_media_dir / path
    dest.parent.mkdir(parents=True, exist_ok=True)
    return str(dest)


def _generate_macos_test_video(row: list[str], output_path: str) -> None:
    cmd = row[1].strip().upper()
    frames = int(row[3]) if len(row) >= 4 and row[3].strip() else 9000
    fps = int(row[4]) if len(row) >= 5 and row[4].strip() else 120
    width = int(row[5]) if len(row) >= 6 and row[5].strip() else 64
    height = int(row[6]) if len(row) >= 7 and row[6].strip() else 64
    pts_offset_us = int(row[7]) if len(row) >= 8 and row[7].strip() else 0
    video_codec = row[8].strip().lower() if len(row) >= 9 and row[8].strip() else "h264"
    with_audio = cmd == "GENERATE_TEST_VIDEO_WITH_AUDIO"
    if frames <= 0 or fps <= 0 or width <= 0 or height <= 0 or pts_offset_us < 0:
        raise ValueError(f"invalid GENERATE_TEST_VIDEO row: {row}")
    if video_codec not in {"h264", "h26410", "vp9", "mpeg2"}:
        raise ValueError(f"unsupported macOS generated UI video codec: {video_codec}")

    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found in PATH for macOS generated UI media")

    output = Path(output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    duration_seconds = frames / fps
    ffmpeg_cmd = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        f"testsrc2=size={width}x{height}:rate={fps}",
    ]
    if with_audio:
        ffmpeg_cmd.extend([
            "-f",
            "lavfi",
            "-i",
            f"sine=frequency=440:sample_rate=48000:duration={duration_seconds}",
        ])
    if pts_offset_us > 0:
        ffmpeg_cmd.extend([
            "-vf",
            f"setpts=PTS+{pts_offset_us / 1000000.0}/TB",
            "-avoid_negative_ts",
            "disabled",
        ])
    ffmpeg_cmd.extend(["-frames:v", str(frames)])
    if with_audio:
        ffmpeg_cmd.extend(["-map", "0:v:0", "-map", "1:a:0"])
    ffmpeg_cmd.extend([
        "-metadata",
        "comment=voidplayer-mac-ui-generated",
    ])
    pixel_format = "yuv420p"
    if video_codec == "vp9":
        ffmpeg_cmd.extend([
            "-c:v",
            "libvpx-vp9",
            "-deadline",
            "realtime",
            "-cpu-used",
            "8",
            "-row-mt",
            "1",
            "-b:v",
            "800k",
        ])
    elif video_codec == "h26410":
        pixel_format = "yuv420p10le"
        ffmpeg_cmd.extend([
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-profile:v",
            "high10",
        ])
    elif video_codec == "mpeg2":
        ffmpeg_cmd.extend([
            "-c:v",
            "mpeg2video",
            "-q:v",
            "4",
        ])
    else:
        ffmpeg_cmd.extend([
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
        ])
    ffmpeg_cmd.extend([
        "-g",
        str(fps),
        "-pix_fmt",
        pixel_format,
    ])
    if with_audio:
        ffmpeg_cmd.extend(["-c:a", "aac", "-b:a", "96k", "-shortest"])
    else:
        ffmpeg_cmd.append("-an")
    ffmpeg_cmd.append(str(output))

    print(f"Generating macOS UI media: {output}")
    result = subprocess.run(
        ffmpeg_cmd,
        cwd=str(ROOT),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"ffmpeg failed ({result.returncode}) generating {output}: "
            f"{result.stderr}"
        )
    if not output.exists() or output.stat().st_size <= 0:
        raise RuntimeError(f"generated macOS UI media is empty: {output}")


def _macos_sandbox_media_path(media_path: str, container_media_dir: Path) -> str:
    media_path = media_path.strip()
    if media_path.startswith("macos-synthetic://"):
        return media_path

    path = Path(media_path)
    resolved = path if path.is_absolute() else (ROOT / path)
    try:
        resolved = resolved.resolve()
    except OSError:
        return media_path

    if not resolved.is_file() or not resolved.is_relative_to(ROOT):
        return media_path

    dest = container_media_dir / resolved.relative_to(ROOT)
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(resolved, dest)
    return str(dest)


def _run_ui_test_process(cmd: list[str]) -> tuple[int, bool]:
    ax_tree_error = False
    test_runner_failed = False
    test_runner_quit = False
    process = subprocess.Popen(
        cmd,
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    assert process.stdout is not None
    for line in process.stdout:
        print(line, end="")
        line_result = _scan_ui_test_output(line)
        if line_result[0]:
            ax_tree_error = True
        if line_result[1]:
            test_runner_failed = True
        if line_result[2]:
            test_runner_quit = True
    result = process.wait()
    if (test_runner_failed or not test_runner_quit) and result == 0:
        result = 1
    return result, ax_tree_error


def _run_macos_ui_test_process(
    app_bundle: Path,
    app_args: list[str],
) -> tuple[int, bool]:
    ax_tree_error = False
    test_runner_failed = False
    test_runner_quit = False
    with tempfile.TemporaryDirectory(prefix="voidplayer-mac-ui-") as tmp:
        output_path = Path(tmp) / "app.log"
        open_cmd = [
            "/usr/bin/open",
            "-W",
            "-n",
            "-g",
            "-o",
            str(output_path),
            "--stderr",
            str(output_path),
            str(app_bundle),
            "--args",
            *app_args,
        ]
        process = subprocess.Popen(open_cmd, cwd=str(ROOT))
        offset = 0
        while process.poll() is None:
            if output_path.exists():
                with output_path.open("r", encoding="utf-8", errors="replace") as file:
                    file.seek(offset)
                    for line in file:
                        print(line, end="")
                        line_result = _scan_ui_test_output(line)
                        if line_result[0]:
                            ax_tree_error = True
                        if line_result[1]:
                            test_runner_failed = True
                        if line_result[2]:
                            test_runner_quit = True
                    offset = file.tell()
            time.sleep(0.05)

        if output_path.exists():
            with output_path.open("r", encoding="utf-8", errors="replace") as file:
                file.seek(offset)
                for line in file:
                    print(line, end="")
                    line_result = _scan_ui_test_output(line)
                    if line_result[0]:
                        ax_tree_error = True
                    if line_result[1]:
                        test_runner_failed = True
                    if line_result[2]:
                        test_runner_quit = True

        result = process.returncode or 0
        if (test_runner_failed or not test_runner_quit) and result == 0:
            result = 1
        return result, ax_tree_error


def _scan_ui_test_output(line: str) -> tuple[bool, bool, bool]:
    ax_tree_error = (
        "Failed to update ui::AXTree" in line or "accessibility_bridge.cc" in line
    )
    test_runner_failed = (
        "TestRunner FAIL" in line or "TestRunner: script ended without QUIT" in line
    )
    test_runner_quit = "TestRunner " in line and (
        ": QUIT " in line or ": CLOSE_MAIN_WINDOW" in line
    )
    return ax_tree_error, test_runner_failed, test_runner_quit
