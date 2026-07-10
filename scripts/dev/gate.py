"""Named validation gate profiles for VoidPlayer development."""

import argparse
import sys

from .check_flutter_fork_protection import check_flutter_fork_protection
from .check_macos_platform_protection import check_macos_platform_protection
from .check_windows_rebuild_boundary import check_windows_rebuild_boundary
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
            run([sys.executable, "scripts/dev/check_release_compliance.py"], cwd=str(ROOT))
        else:
            _python_dev("test", "--native-only")
        return

    if profile == "repo-hygiene":
        cmd_repo_hygiene(argparse.Namespace())
        return

    if profile == "windows-rebuild-boundary":
        errors = check_windows_rebuild_boundary()
        if errors:
            print("Windows rebuild boundary check failed:")
            for error in errors:
                print(f"  - {error}")
            sys.exit(1)
        print("Windows rebuild boundary check passed.")
        return

    if profile == "flutter-fork-protection":
        errors = check_flutter_fork_protection()
        if errors:
            print("Flutter fork protection check failed:")
            for error in errors:
                print(f"  - {error}")
            sys.exit(1)
        print("Flutter fork protection check passed.")
        return

    if profile == "macos-platform-protection":
        errors = check_macos_platform_protection()
        if errors:
            print("macOS platform protection check failed:")
            for error in errors:
                print(f"  - {error}")
            sys.exit(1)
        print("macOS platform protection check passed.")
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
            _python_dev("test", "--native-only", "--github")
            _python_dev("package")
            run([sys.executable, "scripts/dev/check_release_compliance.py"], cwd=str(ROOT))
        else:
            _python_dev("test", "--native-only")
        return

    print(f"ERROR: unknown gate profile: {profile}")
    sys.exit(1)
