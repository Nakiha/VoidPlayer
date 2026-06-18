"""Named validation gate profiles for VoidPlayer development."""

import argparse
import os
import shutil
import sys

from .paths import ROOT
from .process import header, run
from .repo_hygiene import cmd_repo_hygiene


PROFILE_DIR = ROOT / "ui_tests" / "profiles"


def _load_ui_profile(name: str, seen: set[str] | None = None) -> list[str]:
    if seen is None:
        seen = set()
    if name in seen:
        raise RuntimeError(f"recursive UI test profile include: {name}")
    seen.add(name)
    path = PROFILE_DIR / f"{name}.txt"
    scripts: list[str] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("@"):
            scripts.extend(_load_ui_profile(line[1:], seen))
        else:
            scripts.append(line)
    return scripts


def _python_dev(*args: str) -> None:
    run([sys.executable, str(ROOT / "dev.py"), *args], cwd=str(ROOT))


def _python_dev_with_env(environment: dict[str, str], *args: str) -> None:
    child_environment = os.environ.copy()
    child_environment.update(environment)
    run(
        [sys.executable, str(ROOT / "dev.py"), *args],
        cwd=str(ROOT),
        env=child_environment,
    )


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
    _python_dev("mac-ui-test", "--build", *_load_ui_profile("macos-ui-smoke"))


def _run_macos_ui_nightly() -> None:
    _python_dev("mac-ui-test", "--build", *_load_ui_profile("macos-ui-nightly"))


def _run_macos_hdr_edr_smoke() -> None:
    _python_dev("mac-ui-test", "--build", *_load_ui_profile("macos-hdr-edr-smoke"))


def _run_windows_preservation() -> None:
    _python_dev("test", "--native-only")
    local_engine_src = os.environ.get(
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH",
        str(ROOT / ".toolchains" / "flutter" / "engine" / "src"),
    )
    local_engine_environment = {
        "VOIDPLAYER_WINDOWS_PRESENTATION_MODE": "auto",
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH": local_engine_src,
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE": os.environ.get(
            "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE",
            "host_release",
        ),
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE": os.environ.get(
            "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE",
            "host_release",
        ),
    }
    _python_dev_with_env(
        local_engine_environment,
        "ui-test",
        "--build",
        *_load_ui_profile("windows-preservation-auto"),
    )
    _python_dev_with_env(
        {
            **local_engine_environment,
            "VOIDPLAYER_WINDOWS_PRESENTATION_MODE":
                "native-compositor-scrgb",
        },
        "ui-test",
        *_load_ui_profile("windows-preservation-scrgb"),
    )
    _python_dev_with_env(
        {
            **local_engine_environment,
            "VOIDPLAYER_WINDOWS_PRESENTATION_MODE": "sdr",
        },
        "ui-test",
        *_load_ui_profile("windows-preservation-sdr"),
    )


def _generate_windows_hdr_auto_media() -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("ERROR: ffmpeg is required for the Windows HDR Auto gate.")
        sys.exit(1)
    output_dir = ROOT / "build" / "generated" / "windows"
    output_dir.mkdir(parents=True, exist_ok=True)
    common = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        "testsrc2=size=640x360:rate=30",
        "-frames:v",
        "120",
        "-g",
        "30",
    ]
    run(
        [
            *common,
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-pix_fmt",
            "yuv420p",
            str(output_dir / "hdr_auto_sdr.mp4"),
        ],
        cwd=str(ROOT),
    )
    run(
        [
            *common,
            "-c:v",
            "libx265",
            "-preset",
            "ultrafast",
            "-x265-params",
            (
                "log-level=error:hdr10=1:repeat-headers=1:"
                "colorprim=bt2020:transfer=arib-std-b67:"
                "colormatrix=bt2020nc"
            ),
            "-color_primaries",
            "bt2020",
            "-colorspace",
            "bt2020nc",
            "-color_trc",
            "arib-std-b67",
            "-pix_fmt",
            "yuv420p10le",
            str(output_dir / "hdr_auto_hlg.mp4"),
        ],
        cwd=str(ROOT),
    )


def _run_windows_hdr_auto() -> None:
    _generate_windows_hdr_auto_media()
    local_engine_src = os.environ.get(
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH",
        str(ROOT / ".toolchains" / "flutter" / "engine" / "src"),
    )
    _python_dev_with_env(
        {
            "VOIDPLAYER_WINDOWS_PRESENTATION_MODE": "auto",
            "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH": local_engine_src,
            "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE": os.environ.get(
                "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE",
                "host_release",
            ),
            "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE": os.environ.get(
                "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE",
                "host_release",
            ),
        },
        "ui-test",
        "--build",
        "ui_tests/smoke/windows_hdr_auto_runtime.csv",
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


def _run_windows_d3d11_fp16_scrgb_smoke() -> None:
    run(
        [
            "ctest",
            "--test-dir",
            "build/native/standalone/windows-msvc",
            "--build-config",
            "Release",
            "--output-on-failure",
            "-R",
            "^windows_d3d11_fp16_scrgb_smoke$",
        ],
        cwd=str(ROOT),
    )


def _run_windows_d3d11_dcomp_flutter_composite_smoke() -> None:
    run(
        [
            "ctest",
            "--test-dir",
            "build/native/standalone/windows-msvc",
            "--build-config",
            "Release",
            "--output-on-failure",
            "-R",
            "^windows_d3d11_dcomp_flutter_composite_smoke$",
        ],
        cwd=str(ROOT),
    )


def _run_windows_d3d11_source_projection_smoke() -> None:
    run(
        [
            "ctest",
            "--test-dir",
            "build/native/standalone/windows-msvc",
            "--build-config",
            "Release",
            "--output-on-failure",
            "-R",
            "^windows_d3d11_source_projection_smoke$",
        ],
        cwd=str(ROOT),
    )


def _run_windows_d3d11_high_refresh_projection_overlay_smoke() -> None:
    run(
        [
            "ctest",
            "--test-dir",
            "build/native/standalone/windows-msvc",
            "--build-config",
            "Release",
            "--output-on-failure",
            "-R",
            "^windows_d3d11_high_refresh_projection_overlay_smoke$",
        ],
        cwd=str(ROOT),
    )


def _run_windows_d3d11_retained_overlay_layer_smoke() -> None:
    run(
        [
            "ctest",
            "--test-dir",
            "build/native/standalone/windows-msvc",
            "--build-config",
            "Release",
            "--output-on-failure",
            "-R",
            "^windows_d3d11_retained_overlay_layer_smoke$",
        ],
        cwd=str(ROOT),
    )


def _run_windows_display_tests() -> None:
    run(
        [
            str(
                ROOT
                / "build/native/standalone/windows-msvc/Release"
                / "video_renderer_tests.exe"
            ),
            "[windows_display]",
        ],
        cwd=str(ROOT),
    )


def _run_windows_cross_adapter_tests() -> None:
    run(
        [
            str(
                ROOT
                / "build/native/standalone/windows-msvc/Release"
                / "video_renderer_tests.exe"
            ),
            "[windows_cross_adapter]",
        ],
        cwd=str(ROOT),
    )


def _run_windows_device_recovery_tests() -> None:
    run(
        [
            str(
                ROOT
                / "build/native/standalone/windows-msvc/Release"
                / "video_renderer_tests.exe"
            ),
            "[windows_device_recovery]",
        ],
        cwd=str(ROOT),
    )


def _run_windows_high_refresh_tests() -> None:
    run(
        [
            str(
                ROOT
                / "build/native/standalone/windows-msvc/Release"
                / "video_renderer_tests.exe"
            ),
            "[windows_high_refresh]",
        ],
        cwd=str(ROOT),
    )


def _run_windows_overlay_layer_tests() -> None:
    run(
        [
            str(
                ROOT
                / "build/native/standalone/windows-msvc/Release"
                / "video_renderer_tests.exe"
            ),
            "[windows_overlay_layer]",
        ],
        cwd=str(ROOT),
    )


def _run_windows_cross_adapter_local() -> None:
    _run_windows_cross_adapter_tests()
    local_engine_src = os.environ.get(
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH",
        str(ROOT / ".toolchains" / "flutter" / "engine" / "src"),
    )
    local_engine_environment = {
        "VOIDPLAYER_WINDOWS_PRESENTATION_MODE": "auto",
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH": local_engine_src,
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE": os.environ.get(
            "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE",
            "host_release",
        ),
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE": os.environ.get(
            "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE",
            "host_release",
        ),
    }
    _python_dev_with_env(
        {
            **local_engine_environment,
            "VOIDPLAYER_WINDOWS_CROSS_ADAPTER_SYNC": "event-query",
        },
        "ui-test",
        "--build",
        "ui_tests/smoke/native_compositor_auto_sdr.csv",
    )
    _python_dev_with_env(
        {
            **local_engine_environment,
            "VOIDPLAYER_WINDOWS_CROSS_ADAPTER_SYNC": "shared-fence",
        },
        "ui-test",
        "ui_tests/smoke/native_compositor_auto_sdr.csv",
    )


def _run_windows_high_refresh_local() -> None:
    _run_windows_high_refresh_tests()
    _run_windows_overlay_layer_tests()
    _run_windows_d3d11_high_refresh_projection_overlay_smoke()
    _run_windows_d3d11_retained_overlay_layer_smoke()
    local_engine_src = os.environ.get(
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH",
        str(ROOT / ".toolchains" / "flutter" / "engine" / "src"),
    )
    local_engine_environment = {
        "VOIDPLAYER_WINDOWS_PRESENTATION_MODE": "native-compositor-scrgb",
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_SRC_PATH": local_engine_src,
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE": os.environ.get(
            "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_RELEASE",
            "host_release",
        ),
        "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE": os.environ.get(
            "VOIDPLAYER_FLUTTER_LOCAL_ENGINE_HOST_RELEASE",
            "host_release",
        ),
    }
    _python_dev_with_env(
        local_engine_environment,
        "ui-test",
        "--build",
        *_load_ui_profile("windows-high-refresh-local"),
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
        cmd_repo_hygiene(argparse.Namespace())
        if _is_macos():
            _run_macos_native_fast()
        elif _is_windows():
            _python_dev("test", "--native-only", "--github")
            _run_windows_display_tests()
            _run_windows_cross_adapter_tests()
            _run_windows_device_recovery_tests()
            _run_windows_high_refresh_tests()
            _run_windows_overlay_layer_tests()
            _run_windows_d3d11_color_layout_parity_smoke()
            _run_windows_d3d11_fp16_scrgb_smoke()
            _run_windows_d3d11_dcomp_flutter_composite_smoke()
            _run_windows_d3d11_source_projection_smoke()
            _run_windows_d3d11_high_refresh_projection_overlay_smoke()
            _run_windows_d3d11_retained_overlay_layer_smoke()
            run([sys.executable, "scripts/dev/check_release_compliance.py"], cwd=str(ROOT))
        else:
            _python_dev("test", "--native-only")
        return

    if profile == "repo-hygiene":
        cmd_repo_hygiene(argparse.Namespace())
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

    if profile == "windows-hdr-auto":
        if not _is_windows():
            _unsupported(profile, "Windows with HDR enabled")
        _run_windows_hdr_auto()
        return

    if profile == "windows-cross-adapter-local":
        if not _is_windows():
            _unsupported(profile, "Windows with multiple GPU outputs")
        _run_windows_cross_adapter_local()
        return

    if profile == "windows-high-refresh-local":
        if not _is_windows():
            _unsupported(profile, "Windows with a high-refresh display")
        _run_windows_high_refresh_local()
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
