"""Guard the intentionally empty Windows native presentation boundary."""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from .paths import ROOT
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from scripts.dev.paths import ROOT


REMOVED_PATHS = [
    "windows/runner/windows_native_compositor.cpp",
    "windows/runner/flutter_texture_bridge.cpp",
    "native/windows/decode/d3d12va_provider.cpp",
    "native/windows/presentation/windows_d3d12_present_target.cpp",
    "native/windows/presentation/windows_dcomp_composite.cpp",
    "native/windows/shared/shared_texture_ring_types.cpp",
    "native/renderer/exports/ffi_exports.cpp",
]

REQUIRED_TOKENS = {
    "windows/runner/video_renderer_plugin.cpp": [
        '"BACKEND_UNAVAILABLE"',
        "windows-native-unavailable",
        "Windows native presentation backend has not been rebuilt",
    ],
    "native/windows/presentation/windows_presentation_backend.cpp": [
        "create_windows_presentation_backend",
        "return nullptr;",
    ],
    "native/cmake/NativeSourcesWindows.cmake": [
        "windows/presentation/windows_presentation_backend.cpp",
    ],
}

FORBIDDEN_RUNNER_TOKENS = [
    "dcomp.lib",
    "d3d12",
    "FlutterNativeTarget",
    "windows_native_compositor.cpp",
]


def check_windows_rebuild_boundary() -> list[str]:
    errors: list[str] = []
    for rel in REMOVED_PATHS:
        if (ROOT / rel).exists():
            errors.append(f"removed Windows implementation returned: {rel}")
    for rel, tokens in REQUIRED_TOKENS.items():
        path = ROOT / rel
        if not path.is_file():
            errors.append(f"missing Windows rebuild boundary: {rel}")
            continue
        text = path.read_text(encoding="utf-8")
        for token in tokens:
            if token not in text:
                errors.append(f"{rel} is missing boundary token: {token}")
    runner_cmake = (ROOT / "windows/runner/CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    for token in FORBIDDEN_RUNNER_TOKENS:
        if token in runner_cmake:
            errors.append(f"Windows runner restored forbidden dependency: {token}")
    return errors


def main() -> int:
    errors = check_windows_rebuild_boundary()
    if errors:
        print("Windows rebuild boundary check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("Windows rebuild boundary check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
