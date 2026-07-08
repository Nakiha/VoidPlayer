"""Static guardrails for the macOS standard Flutter Texture presentation line."""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from .paths import ROOT
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from scripts.dev.paths import ROOT


FORBIDDEN_PRODUCT_PATHS = [
    "native/rust",
    "native/cmake/WgpuRustTarget.cmake",
    "native/cmake/BuildWgpuRust.cmake",
    "native/macos/wgpu",
]

FORBIDDEN_SOURCE_MARKERS = [
    "RendererBackendType::WgpuMetal",
    "RenderBackendKind::WgpuMetal",
    "voidplayer_wgpu_ffi",
    "macos_wgpu_metal_presentation_backend_smoke",
]

REQUIRED_SOURCE_MARKERS = {
    "macos/Runner/MacOSPresentationConfiguration.swift": [
        "case flutterTextureSDR = \"flutter-texture-sdr\"",
        "auto-hdr-deferred-flutter-texture-sdr",
    ],
    "native/cmake/NativeSourcesMacOS.cmake": [
        "macos/metal/metal_presentation_backend.cpp",
        "macos/decode/videotoolbox_provider.cpp",
    ],
    "native/macos/decode/videotoolbox_provider.cpp": [
        "VideoToolbox",
        "AV_PIX_FMT_VIDEOTOOLBOX",
    ],
}


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def check_macos_platform_protection() -> list[str]:
    errors: list[str] = []

    for rel in FORBIDDEN_PRODUCT_PATHS:
        if (ROOT / rel).exists():
            errors.append(f"macOS WGPU product path should be removed: {rel}")

    searchable = "\n".join(
        _read(rel)
        for rel in [
            "scripts/dev/gate.py",
            "native/cmake/NativeSourcesMacOS.cmake",
            "native/cmake/MacOSTargets.cmake",
            "native/cmake/MacOSTests.cmake",
            "native/renderer/render/backend_type.h",
        ]
    )
    for marker in FORBIDDEN_SOURCE_MARKERS:
        if marker in searchable:
            errors.append(f"macOS WGPU product marker should be removed: {marker}")

    for rel, markers in REQUIRED_SOURCE_MARKERS.items():
        path = ROOT / rel
        if not path.is_file():
            errors.append(f"missing macOS protection file: {rel}")
            continue
        text = path.read_text(encoding="utf-8")
        for marker in markers:
            if marker not in text:
                errors.append(f"{rel} is missing macOS protection marker: {marker}")

    return errors


def main() -> int:
    errors = check_macos_platform_protection()
    if errors:
        print("macOS platform protection check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("macOS platform protection check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
