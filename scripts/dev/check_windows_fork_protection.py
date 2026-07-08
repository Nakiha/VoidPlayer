"""Static guardrails for the Windows standard Flutter Texture presentation line."""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from .paths import ROOT
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from scripts.dev.paths import ROOT


REQUIRED_GATE_CALLS = [
    "_run_windows_preservation()",
]

REQUIRED_UI_PROFILE_ENTRIES = {
    "ui_tests/profiles/windows-preservation-auto.txt": [
        "ui_tests/smoke/basic.csv",
        "ui_tests/smoke/native_seek_preview_event.csv",
    ],
    "ui_tests/profiles/windows-preservation-scrgb.txt": [
        "ui_tests/smoke/basic.csv",
        "ui_tests/smoke/native_seek_preview_event.csv",
    ],
    "ui_tests/profiles/windows-preservation-sdr.txt": [
        "ui_tests/smoke/basic.csv",
        "ui_tests/smoke/native_seek_preview_event.csv",
    ],
}

REQUIRED_NATIVE_TAGS = {
    "[windows_display]": "native/tests/windows/test_windows_display_resolver.cpp",
}


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def check_windows_fork_protection() -> list[str]:
    errors: list[str] = []

    gate = _read("scripts/dev/gate.py")
    for call in REQUIRED_GATE_CALLS:
        if call not in gate:
            errors.append(f"scripts/dev/gate.py is missing Windows protection call: {call}")

    test_cmake = _read("native/tests/CMakeLists.txt")
    for tag, rel in REQUIRED_NATIVE_TAGS.items():
        path = ROOT / rel
        if not path.is_file():
            errors.append(f"missing Windows protection test file: {rel}")
            continue
        if tag not in path.read_text(encoding="utf-8"):
            errors.append(f"{rel} is missing Catch2 tag {tag}")
        cmake_rel = Path(rel).relative_to("native/tests").as_posix()
        if cmake_rel not in test_cmake:
            errors.append(f"native/tests/CMakeLists.txt does not include {cmake_rel}")

    for profile, entries in REQUIRED_UI_PROFILE_ENTRIES.items():
        path = ROOT / profile
        if not path.is_file():
            errors.append(f"missing Windows preservation UI profile: {profile}")
            continue
        lines = {
            line.strip()
            for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.strip().startswith("#")
        }
        for entry in entries:
            if entry not in lines:
                errors.append(f"{profile} is missing UI smoke: {entry}")
            elif not (ROOT / entry).is_file():
                errors.append(f"{profile} references missing UI smoke: {entry}")

    return errors


def main() -> int:
    errors = check_windows_fork_protection()
    if errors:
        print("Windows fork protection check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("Windows fork protection check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
