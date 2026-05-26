"""Named validation gate profiles for VoidPlayer development."""

import argparse
import sys

from .paths import ROOT
from .process import header, run


MACOS_UI_SMOKE = [
    "ui_tests/macos/native_facade_smoke.csv",
    "ui_tests/macos/native_seek_frame_smoke.csv",
    "ui_tests/macos/native_layout_split_smoke.csv",
    "ui_tests/macos/native_controls_smoke.csv",
]

MACOS_UI_NIGHTLY = [
    *MACOS_UI_SMOKE,
    "ui_tests/macos/native_audio_play_seek_smoke.csv",
    "ui_tests/macos/native_audio_destroy_recreate_smoke.csv",
    "ui_tests/macos/native_4k60_playback_smoke.csv",
    "ui_tests/macos/native_vvc_software_playback_smoke.csv",
    "ui_tests/macos/native_p010_presentation_smoke.csv",
    "ui_tests/macos/native_callback_stress_smoke.csv",
    "ui_tests/macos/native_user_window_close_smoke.csv",
    "ui_tests/macos/native_quit_while_playing_smoke.csv",
    "ui_tests/macos/native_loop_range_smoke.csv",
    "ui_tests/macos/native_eof_settle_smoke.csv",
    "ui_tests/macos/native_playing_seek_keeps_state_smoke.csv",
    "ui_tests/macos/native_playing_step_pauses_smoke.csv",
    "ui_tests/macos/native_seek_preview_event_smoke.csv",
    "ui_tests/macos/analysis_gated_smoke.csv",
]


def _python_dev(*args: str) -> None:
    run([sys.executable, str(ROOT / "dev.py"), *args], cwd=str(ROOT))


def _is_macos() -> bool:
    return sys.platform == "darwin"


def _is_windows() -> bool:
    return sys.platform == "win32"


def _unsupported(profile: str, platform: str) -> None:
    print(f"ERROR: dev.py gate {profile} is only supported on {platform}.")
    sys.exit(1)


def _run_macos_native_fast() -> None:
    run(["bash", "scripts/ci/run_macos_native_fast.sh"], cwd=str(ROOT))


def _run_macos_ui_smoke() -> None:
    _python_dev("mac-ui-test", "--build", *MACOS_UI_SMOKE)


def _run_macos_ui_nightly() -> None:
    _python_dev("mac-ui-test", "--build", *MACOS_UI_NIGHTLY)


def _run_windows_preservation() -> None:
    _python_dev("test", "--native-only")
    run(["flutter", "build", "windows", "--release"], cwd=str(ROOT))
    _python_dev("ui-test", "--build", "ui_tests/smoke/basic.csv")


def cmd_gate(args: argparse.Namespace) -> None:
    """Run a named validation gate profile."""
    profile = args.profile
    header(f"Gate profile: {profile}")

    if profile == "pr-fast":
        if _is_macos():
            _run_macos_native_fast()
        elif _is_windows():
            _python_dev("test", "--native-only", "--github")
            run([sys.executable, "scripts/dev/check_release_compliance.py"], cwd=str(ROOT))
        else:
            _python_dev("test", "--native-only")
        return

    if profile == "macos-native-fast":
        if not _is_macos():
            _unsupported(profile, "macOS")
        _run_macos_native_fast()
        return

    if profile == "macos-ui-smoke":
        if not _is_macos():
            _unsupported(profile, "macOS")
        _run_macos_ui_smoke()
        return

    if profile == "macos-ui-nightly":
        if not _is_macos():
            _unsupported(profile, "macOS")
        _run_macos_ui_nightly()
        return

    if profile == "windows-preservation":
        if not _is_windows():
            _unsupported(profile, "Windows")
        _run_windows_preservation()
        return

    if profile == "release-candidate":
        if _is_macos():
            _run_macos_native_fast()
            _python_dev("build", "--flutter")
            _run_macos_ui_nightly()
            _python_dev("package")
            run(
                [
                    sys.executable,
                    "scripts/dev/check_release_compliance.py",
                    "--stage",
                    "build/package/macos/VoidPlayer",
                ],
                cwd=str(ROOT),
            )
        elif _is_windows():
            _run_windows_preservation()
            _python_dev("package")
            run([sys.executable, "scripts/dev/check_release_compliance.py"], cwd=str(ROOT))
        else:
            _python_dev("test", "--native-only")
        return

    print(f"ERROR: unknown gate profile: {profile}")
    sys.exit(1)
