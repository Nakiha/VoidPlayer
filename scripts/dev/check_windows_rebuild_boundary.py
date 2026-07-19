"""Guard the active Windows native presentation rebuild boundary.

D3D11VA decode, typed frame storage, target rings, and a standalone D3D11
presentation backend are active. The runner owns the shared native player and
consumes the locked Flutter V1 lease in a passive DComp compositor. This check
prevents the active player path from losing those boundaries or restoring the
deleted capture/D3D12 paths.
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from .paths import ROOT
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from scripts.dev.paths import ROOT


REMOVED_PATHS = [
    "windows/runner/flutter_texture_bridge.cpp",
    "native/windows/decode/d3d12va_provider.cpp",
    "native/windows/presentation/windows_d3d12_present_target.cpp",
    "native/windows/presentation/windows_dcomp_composite.cpp",
    "native/windows/shared/shared_texture_ring_types.cpp",
    "native/renderer/exports/ffi_exports.cpp",
]

REQUIRED_TOKENS = {
    "windows/CMakeLists.txt": [
        "VideoDecodeCoreTarget.cmake",
    ],
    "windows/runner/video_renderer_plugin.cpp": [
        "WindowsNativePlayer",
        "install_target_ring",
        "target_release_queue_.Enqueue",
        "windows-native-d3d11",
    ],
    "windows/runner/windows_target_release_queue.h": [
        "WindowsTargetReleaseQueue",
        "Drain",
    ],
    "windows/runner/renderer_event_bridge.h": [
        "RendererEventBridge",
        "PostTask",
        "pending_tasks_",
    ],
    "native/windows/presentation/windows_presentation_backend.cpp": [
        "create_windows_presentation_backend",
        "WindowsD3D11PresentationBackend",
    ],
    "native/cmake/NativeSourcesWindows.cmake": [
        "analysis_overlay_renderer_portable",
        "windows/decode/d3d11_frame_snapshot.cpp",
        "windows/player/native_player.cpp",
        "windows/presentation/windows_d3d11_target_ring.cpp",
        "windows/presentation/windows_presentation_backend.cpp",
    ],
    "native/cmake/VideoDecodeCoreTarget.cmake": [
        "media/video_decode_session.cpp",
        "windows/decode/d3d11va_provider.cpp",
        "d3d11 dxgi",
    ],
    "windows/runner/windows_native_compositor.cpp": [
        "FlutterDesktopViewAcquireLatestSurface",
        "OpenSharedResource1",
        "IDXGIKeyedMutex",
        "kFlutterDesktopWindowsSurfaceExportModeCompositorOwned",
    ],
}

FORBIDDEN_RUNNER_TOKENS = [
    "d3d12",
    "FlutterNativeTarget",
]

FORBIDDEN_COMPOSITOR_TOKENS = [
    "FlutterDesktopViewRequestSurfaceExportFrame",
    "WS_EX_LAYERED",
    "PrintWindow",
    "BitBlt",
    "D3D12",
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
    compositor = (ROOT / "windows/runner/windows_native_compositor.cpp").read_text(
        encoding="utf-8"
    )
    for token in FORBIDDEN_COMPOSITOR_TOKENS:
        if token in compositor:
            errors.append(f"Windows compositor restored forbidden path: {token}")
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
