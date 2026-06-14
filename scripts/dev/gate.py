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
    "ui_tests/macos/native_compositor_auto_sdr_policy_smoke.csv",
]

MACOS_HDR_EDR_SMOKE = [
    "ui_tests/macos/native_compositor_auto_hlg_policy_smoke.csv",
    "ui_tests/macos/native_compositor_add_hlg_promotes_edr_smoke.csv",
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
    "ui_tests/macos/native_add_short_after_eof_smoke.csv",
    "ui_tests/macos/native_h264_high422_fallback_smoke.csv",
    "ui_tests/macos/native_odd_yuv_format_smoke.csv",
    "ui_tests/macos/native_playing_seek_keeps_state_smoke.csv",
    "ui_tests/macos/native_playing_step_pauses_smoke.csv",
    "ui_tests/macos/native_seek_preview_event_smoke.csv",
    "ui_tests/macos/analysis_gated_smoke.csv",
    "ui_tests/macos/analysis_av1_overlay_unsupported_smoke.csv",
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


def _run_macos_native_werror() -> None:
    run(["cmake", "--preset", "macos-werror"], cwd=str(ROOT / "native"))
    run(["cmake", "--build", "--preset", "macos-werror"], cwd=str(ROOT / "native"))


def _run_macos_native_sanitizer(preset: str) -> None:
    run(["cmake", "--preset", preset], cwd=str(ROOT / "native"))
    run(["cmake", "--build", "--preset", preset], cwd=str(ROOT / "native"))
    run(
        [
            "ctest",
            "--preset",
            preset,
            "--label-exclude",
            "hosted-flaky|nightly",
        ],
        cwd=str(ROOT / "native"),
    )


def _run_macos_native_sanitizers() -> None:
    _run_macos_native_sanitizer("macos-asan")
    _run_macos_native_sanitizer("macos-tsan")


def _run_macos_ui_smoke() -> None:
    _python_dev("mac-ui-test", "--build", *MACOS_UI_SMOKE)


def _run_macos_ui_nightly() -> None:
    _python_dev("mac-ui-test", "--build", *MACOS_UI_NIGHTLY)


def _run_macos_hdr_edr_smoke() -> None:
    _python_dev("mac-ui-test", "--build", *MACOS_HDR_EDR_SMOKE)


def _run_windows_preservation() -> None:
    _python_dev("test", "--native-only")
    _python_dev(
        "ui-test",
        "--build",
        "ui_tests/smoke/basic.csv",
        "ui_tests/smoke/native_seek_preview_event.csv",
    )


def _run_windows_d3d11_color_layout_parity_smoke() -> None:
    run(
        [
            "ctest",
            "--test-dir",
            "build/native/standalone/windows-msvc",
            "--build-config",
            "Release",
            "--output-on-failure",
            "-R",
            "^windows_d3d11_color_layout_parity_smoke$",
        ],
        cwd=str(ROOT),
    )


def _run_macos_release_readiness() -> None:
    _python_dev("package")
    run(
        [
            sys.executable,
            "scripts/dev/check_macos_release_readiness.py",
            "--stage",
            "build/package/macos/VoidPlayer",
        ],
        cwd=str(ROOT),
    )


def cmd_gate(args: argparse.Namespace) -> None:
    """Run a named validation gate profile."""
    profile = args.profile
    header(f"Gate profile: {profile}")

    if profile == "pr-fast":
        if _is_macos():
            _run_macos_native_fast()
        elif _is_windows():
            _python_dev("test", "--native-only", "--github")
            _run_windows_d3d11_color_layout_parity_smoke()
            run([sys.executable, "scripts/dev/check_release_compliance.py"], cwd=str(ROOT))
        else:
            _python_dev("test", "--native-only")
        return

    if profile == "macos-native-fast":
        if not _is_macos():
            _unsupported(profile, "macOS")
        _run_macos_native_fast()
        return

    if profile == "macos-native-werror":
        if not _is_macos():
            _unsupported(profile, "macOS")
        _run_macos_native_werror()
        return

    if profile == "macos-native-sanitizers":
        if not _is_macos():
            _unsupported(profile, "macOS")
        _run_macos_native_sanitizers()
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

    if profile == "macos-hdr-edr-smoke":
        if not _is_macos():
            _unsupported(profile, "macOS with an EDR-capable display")
        _run_macos_hdr_edr_smoke()
        return

    if profile == "windows-preservation":
        if not _is_windows():
            _unsupported(profile, "Windows")
        _run_windows_preservation()
        return

    if profile == "macos-release-readiness":
        if not _is_macos():
            _unsupported(profile, "macOS")
        _run_macos_release_readiness()
        return

    if profile == "release-candidate":
        if _is_macos():
            _run_macos_native_fast()
            _run_macos_native_werror()
            _run_macos_native_sanitizers()
            _python_dev("build", "--flutter")
            _run_macos_ui_nightly()
            _run_macos_release_readiness()
        elif _is_windows():
            _run_windows_preservation()
            _python_dev("package")
            run([sys.executable, "scripts/dev/check_release_compliance.py"], cwd=str(ROOT))
        else:
            _python_dev("test", "--native-only")
        return

    print(f"ERROR: unknown gate profile: {profile}")
    sys.exit(1)
