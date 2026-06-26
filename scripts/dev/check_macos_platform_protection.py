"""Static guardrails for the macOS native presentation protection line."""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from .paths import ROOT
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from scripts.dev.paths import ROOT


REQUIRED_GATE_SNIPPETS = [
    "_run_macos_native_fast()",
    "_run_macos_ui_smoke()",
    "_run_macos_hdr_edr_smoke()",
    "_run_macos_wgpu_metal_smoke()",
    "_run_macos_wgpu_metal_edr_smoke()",
    "_run_macos_release_readiness()",
    "macos-platform-protection",
]

REQUIRED_CMAKE_SOURCES = [
    "macos/metal/metal_presentation_backend_bridge.cpp",
    "macos/metal/metal_presentation_backend.cpp",
    "macos/metal/metal_uploader_bridge.mm",
    "macos/metal/metal_texture_wrapping.mm",
    "macos/metal/metal_pixel_buffer_uploader.mm",
    "macos/player/native_player_presentation_target.cpp",
    "macos/presentation/presentation_adapter.cpp",
    "macos/presentation/presentation_cv_pixel_buffer_frame.cpp",
    "macos/presentation/presentation_package_builder.cpp",
    "macos/wgpu/wgpu_ffi_stub.cpp",
    "macos/wgpu/wgpu_metal_presentation_backend.mm",
]

REQUIRED_CMAKE_TARGETS = [
    "void_macos_native_player",
    "target_sources(void_media_ffmpeg PRIVATE",
    "macos/decode/videotoolbox_provider.cpp",
    "macos_metal_uploader_smoke",
    "macos_metal_presentation_backend_smoke",
    "macos_metal_color_layout_parity_smoke",
    "macos_metal_color_reference_smoke",
    "videotoolbox_provider_smoke",
    "renderer_metal_headless_smoke",
    "macos_native_player_shared_renderer_smoke",
    "voidplayer_wgpu_ffi_rust",
]

REQUIRED_SOURCE_MARKERS = {
    "macos/Runner/MacOSFlutterTextureBridge.swift": [
        "protocol MacOSVideoTexture: FlutterTexture",
        "rendererOwnedPixelBufferCount",
        "installNativePresentationTarget",
        "publishRenderedTargetAndInstallNext",
        "kCVPixelBufferMetalCompatibilityKey",
        "kCVPixelBufferIOSurfacePropertiesKey",
        "Unmanaged.passRetained",
    ],
    "macos/Runner/MacOSNativeMetalPresentationTarget.swift": [
        "VPMacOSMetalPresentationBackendCreate",
        "VPMacOSMetalPresentationBackendDestroy",
        "VPMacOSMetalPresentationBackendIsAvailable",
        "VPMacOSMetalPresentationBackendValidatePixelBufferChecked",
        "installMetalPresentationTargetRing",
    ],
    "macos/Runner/MacOSPlaybackController.swift": [
        "lastRendererOwnedPresentationSucceeded",
        "publishRenderedTargetAndInstallNext",
        "VoidPlayer macOS renderer-owned Metal presentation failed",
    ],
    "macos/Runner/MacOSVideoRendererDiagnostics.swift": [
        '"presentationAdapterKind"',
        '"presentationBackend"',
        "native-wgpu-metal-cvpixelbuffer-target",
        "renderer-owned-metal",
        '"nativePresentationTargetInstalled"',
        '"hardwareDecodeProvider"',
        '"presentationUploadMode"',
        '"presentationFallbackReason"',
    ],
    "macos/Runner/MacOSNativeCompositorView.swift": [
        "import IOSurface",
        "currentFlutterMetalTexture",
        "nativeCompositorFlutterTextureAvailable",
        "Flutter surface missing IOSurface",
        "IOSurfaceLock",
    ],
    "native/macos/player/native_player_state.cpp": [
        "RendererBackendType::WgpuMetal",
        "RendererBackendType::Metal",
        "HwDecodeType::VideoToolbox",
        "renderer-owned Metal presentation target is not installed",
        "record_presentation_failure_locked",
    ],
    "native/macos/decode/videotoolbox_provider.cpp": [
        "VideoToolbox",
        "Renderer-owned CVPixelBuffer output requires Metal-compatible backend",
        "AV_PIX_FMT_VIDEOTOOLBOX",
    ],
}

REQUIRED_UI_PROFILE_ENTRIES = {
    "ui_tests/profiles/macos-ui-smoke.txt": [
        "ui_tests/macos/native_facade_smoke.csv",
        "ui_tests/macos/native_seek_frame_smoke.csv",
        "ui_tests/macos/native_layout_split_smoke.csv",
        "ui_tests/macos/native_controls_smoke.csv",
        "ui_tests/macos/native_compositor_auto_sdr_policy_smoke.csv",
    ],
    "ui_tests/profiles/macos-ui-nightly.txt": [
        "@macos-ui-smoke",
        "ui_tests/macos/native_4k60_playback_smoke.csv",
        "ui_tests/macos/native_vvc_software_playback_smoke.csv",
        "ui_tests/macos/native_p010_presentation_smoke.csv",
        "ui_tests/macos/native_h264_high422_fallback_smoke.csv",
        "ui_tests/macos/native_callback_stress_smoke.csv",
    ],
    "ui_tests/profiles/macos-hdr-edr-smoke.txt": [
        "ui_tests/macos/native_compositor_auto_hlg_policy_smoke.csv",
        "ui_tests/macos/native_compositor_add_hlg_promotes_edr_smoke.csv",
    ],
}

REQUIRED_UI_SCRIPT_MARKERS = {
    "ui_tests/macos/native_facade_smoke.csv": [
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, presentationAdapterKind, renderer-owned-metal",
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, presentationBackend, native-wgpu-metal-cvpixelbuffer-target",
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, rendererOwnedBackendName, wgpu-metal",
        "ASSERT_NATIVE_DIAGNOSTIC_BOOL, nativePresentationTargetInstalled, true",
    ],
    "ui_tests/macos/native_compositor_auto_sdr_policy_smoke.csv": [
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, macOSPresentationRequest, auto",
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, macOSPresentationMode, native-compositor-sdr",
        "ASSERT_NATIVE_DIAGNOSTIC_BOOL, macOSPresentationEDROutputEnabled, false",
    ],
    "ui_tests/macos/native_4k60_playback_smoke.csv": [
        "default wgpu-metal VideoToolbox CVPixelBuffer path",
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, presentationBackend, native-wgpu-metal-cvpixelbuffer-target",
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, rendererOwnedBackendName, wgpu-metal",
        "ASSERT_NATIVE_DIAGNOSTIC_INT_AT_LEAST, pixelBufferMetalCVPixelBufferUploadCount",
    ],
    "ui_tests/macos/native_p010_presentation_smoke.csv": [
        "VideoToolbox P010 renderer-owned presentation",
        "VideoToolbox / h264",
        "ASSERT_NATIVE_DIAGNOSTIC_INT_AT_LEAST, pixelBufferMetalCVPixelBufferUploadCount",
    ],
    "ui_tests/macos/native_h264_high422_fallback_smoke.csv": [
        "VideoToolbox direct path",
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, decodeMode, shared-renderer-software",
        "ASSERT_NATIVE_DIAGNOSTIC_BOOL, hardwareDecodeActive, false",
    ],
    "ui_tests/macos/native_vvc_software_playback_smoke.csv": [
        "VideoToolbox currently declines VVC",
        "ASSERT_NATIVE_DIAGNOSTIC_BOOL, nativePresentationTargetInstalled, true",
    ],
    "ui_tests/macos/native_software_fallback_smoke.csv": [
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, hardwareDecodeProvider, VideoToolbox",
        "ASSERT_NATIVE_DIAGNOSTIC_BOOL, softwareFallbackActive, true",
        "ASSERT_NATIVE_DIAGNOSTIC_STRING, decodeMode, shared-renderer-software",
    ],
}


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def _check_contains(
    errors: list[str],
    rel: str,
    markers: list[str],
    *,
    label: str | None = None,
) -> None:
    path = ROOT / rel
    if not path.is_file():
        errors.append(f"missing macOS protection file: {rel}")
        return
    text = path.read_text(encoding="utf-8")
    for marker in markers:
        if marker not in text:
            errors.append(f"{label or rel} is missing macOS protection marker: {marker}")


def _profile_lines(rel: str, errors: list[str]) -> set[str]:
    path = ROOT / rel
    if not path.is_file():
        errors.append(f"missing macOS UI profile: {rel}")
        return set()
    return {
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.strip().startswith("#")
    }


def check_macos_platform_protection() -> list[str]:
    errors: list[str] = []

    _check_contains(errors, "scripts/dev/gate.py", REQUIRED_GATE_SNIPPETS)

    source_list = _read("native/cmake/NativeSourcesMacOS.cmake")
    for rel in REQUIRED_CMAKE_SOURCES:
        if rel not in source_list:
            errors.append(f"native/cmake/NativeSourcesMacOS.cmake is missing {rel}")

    cmake_text = (
        _read("native/cmake/MacOSTargets.cmake")
        + "\n"
        + _read("native/cmake/MacOSTests.cmake")
        + "\n"
        + _read("native/cmake/PortableTargets.cmake")
    )
    for target in REQUIRED_CMAKE_TARGETS:
        if target not in cmake_text:
            errors.append(f"macOS CMake protection target is missing: {target}")
    if "hosted-flaky;nightly" not in cmake_text:
        errors.append("macOS heavyweight Metal/VideoToolbox smokes must stay labelled hosted-flaky;nightly")

    for rel, markers in REQUIRED_SOURCE_MARKERS.items():
        _check_contains(errors, rel, markers)

    for profile, entries in REQUIRED_UI_PROFILE_ENTRIES.items():
        lines = _profile_lines(profile, errors)
        for entry in entries:
            if entry not in lines:
                errors.append(f"{profile} is missing UI smoke: {entry}")
            elif not entry.startswith("@") and not (ROOT / entry).is_file():
                errors.append(f"{profile} references missing UI smoke: {entry}")

    for rel, markers in REQUIRED_UI_SCRIPT_MARKERS.items():
        _check_contains(errors, rel, markers)

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
