"""Flutter app build, launch, demo, and UI test commands."""

import csv
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from .native import ensure_ffmpeg_analyzer_tool, native_build
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
    """Run Flutter unit tests and native standalone tests."""
    if args.flutter_only and args.native_only:
        print("ERROR: --flutter-only and --native-only are mutually exclusive")
        sys.exit(1)

    if not args.native_only:
        flutter_unit_test()

    if not args.flutter_only:
        native_build(args.debug, test=True, github=args.github)


def cmd_ui_test(args) -> None:
    """Launch the app with a CSV script and use process exit code as result."""
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
        if args.visible:
            result, ax_tree_error = _run_ui_test_process([str(exe), *app_args])
        else:
            result, ax_tree_error = _run_macos_ui_test_process(
                app_bundle,
                app_args,
            )
        if ax_tree_error:
            print(f"\nmacOS UI test failed: Flutter AXTree error detected: {label}")
            sys.exit(1)
        if result != 0:
            print(f"\nmacOS UI test failed with exit code {result}: {label}")
            sys.exit(result)

    print(f"\nmacOS UI test batch passed ({total} script{'s' if total != 1 else ''}).")


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
    with_audio = cmd == "GENERATE_TEST_VIDEO_WITH_AUDIO"
    if frames <= 0 or fps <= 0 or width <= 0 or height <= 0 or pts_offset_us < 0:
        raise ValueError(f"invalid GENERATE_TEST_VIDEO row: {row}")

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
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-g",
        str(fps),
        "-pix_fmt",
        "yuv420p",
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
