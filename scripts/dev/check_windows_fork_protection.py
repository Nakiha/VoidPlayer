"""Static guardrails for the disabled Windows native renderer restart line."""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from .paths import ROOT
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from scripts.dev.paths import ROOT


REQUIRED_TOKENS = {
    "windows/runner/video_renderer_plugin.cpp": [
        "WindowsRenderBackendSelection",
        "reserved ",
        "but not implemented",
        '"disabled"',
    ],
    "native/renderer/renderer_config_validation.cpp": [
        "windows native-d3d11 renderer backend is reserved",
        "windows native-d3d12 renderer backend is reserved",
    ],
    "native/renderer/render/presentation_backend_factory.cpp": [
        "#ifdef _WIN32",
        "return false;",
        "return nullptr;",
    ],
    "native/cmake/NativeSourcesWindows.cmake": [
        "windows/common/windows_crash_handler.cpp",
        "windows/player/native_player.cpp",
    ],
}

FORBIDDEN_WINDOWS_ACTIVE_SOURCES = [
    "windows/presentation/windows_dcomp_composite.cpp",
    "windows/presentation/windows_d3d12_present_target.cpp",
    "windows/decode/d3d12va_provider.cpp",
    "windows/shared/shared_texture_ring_types.cpp",
]

FORBIDDEN_GATE_PROFILES = [
    "windows-preservation",
    "windows-hdr-auto",
    "windows-cross-adapter-local",
    "windows-high-refresh-local",
]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def check_windows_fork_protection() -> list[str]:
    errors: list[str] = []

    for rel, tokens in REQUIRED_TOKENS.items():
        text = _read(rel)
        for token in tokens:
            if token not in text:
                errors.append(f"{rel} is missing disabled-backend guard token: {token}")

    cmake = _read("native/cmake/NativeSourcesWindows.cmake")
    for source in FORBIDDEN_WINDOWS_ACTIVE_SOURCES:
        if source in cmake:
            errors.append(f"Windows backend source is still active in CMake: {source}")

    cli = _read("scripts/dev/cli.py")
    gate = _read("scripts/dev/gate.py")
    for profile in FORBIDDEN_GATE_PROFILES:
        if profile in cli or profile in gate:
            errors.append(f"removed Windows gate profile still appears in dev scripts: {profile}")

    for path in (ROOT / "ui_tests" / "profiles").glob("windows-*.txt"):
        errors.append(f"removed Windows UI profile still exists: {path.relative_to(ROOT)}")

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
