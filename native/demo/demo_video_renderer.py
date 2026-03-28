"""Minimal demo: Multi-track video renderer using PySide6 + D3D11."""
import sys
from pathlib import Path

# Add build output directory so video_renderer_native.pyd can be found
_build_dir = Path(__file__).resolve().parent.parent / "build-msvc"
for _cfg in ("Release", "Debug"):
    _candidate = _build_dir / _cfg
    if _candidate.is_dir():
        sys.path.insert(0, str(_candidate))
        break

from PySide6.QtWidgets import QApplication
from PySide6.QtGui import QWindow
from video_renderer_native import Renderer, RendererConfig

VIDEO_DIR = Path(__file__).resolve().parent.parent.parent / "resources" / "video"


def main():
    app = QApplication(sys.argv)

    window = QWindow()
    window.setTitle("VoidPlayer - Video Renderer Demo")
    window.resize(1280, 720)
    window.show()

    # Ensure window is fully created before getting HWND
    app.processEvents()

    config = RendererConfig()
    config.video_paths = [
        str(VIDEO_DIR / "h265_10s_1920x1080.mp4"),
    ]
    config.hwnd = int(window.winId())
    config.width = window.width()
    config.height = window.height()
    config.use_hardware_decode = True

    renderer = Renderer()
    if not renderer.initialize(config):
        print("Failed to initialize renderer")
        return 1

    print(f"Tracks: {renderer.track_count()}")
    print(f"Duration: {renderer.duration_us() / 1_000_000:.1f}s")
    print("Playing...")
    renderer.play()

    ret = app.exec()
    renderer.shutdown()
    return ret


if __name__ == "__main__":
    sys.exit(main())
