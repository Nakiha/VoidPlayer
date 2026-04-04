"""Interactive demo: Multi-track video renderer with keyboard and mouse controls.

Keyboard shortcuts:
  Space        Play / Pause
  Right        Step forward 1 frame
  Left         Step backward 1 frame
  Shift+Right  Seek forward 1 second
  Shift+Left   Seek backward 1 second
  M            Toggle SIDE_BY_SIDE / SPLIT_SCREEN mode

Mouse controls:
  Left drag    Pan view offset
  Wheel        Zoom in/out
  Right drag   Move split divider (SPLIT_SCREEN mode)
"""
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
from PySide6.QtGui import QWindow, QKeyEvent, QMouseEvent, QWheelEvent
from PySide6.QtCore import Qt, QTimer, QPoint
from video_renderer_native import (
    Renderer, RendererConfig, SeekType, LayoutState,
    LAYOUT_SIDE_BY_SIDE, LAYOUT_SPLIT_SCREEN,
)

VIDEO_DIR = Path(__file__).resolve().parent.parent.parent / "resources" / "video"
MODE_NAMES = {LAYOUT_SIDE_BY_SIDE: "SideBySide", LAYOUT_SPLIT_SCREEN: "SplitScreen"}


class VideoWindow(QWindow):
    def __init__(self):
        super().__init__()
        self.setTitle("VoidPlayer - Demo")
        self.resize(1280, 720)

        self.renderer = Renderer()
        self.playing = False

        # Layout state — single struct, applied atomically
        self.layout = LayoutState()

        # Mouse drag state
        self._panning = False
        self._splitting = False
        self._last_mouse_pos = QPoint()

        # PTS overlay refresh
        self.overlay_timer = QTimer(self)
        self.overlay_timer.setInterval(100)
        self.overlay_timer.timeout.connect(self._update_overlay)

    def init_renderer(self, video_paths: list[str] | None = None):
        if video_paths is None:
            default_video = str(VIDEO_DIR / "h265_10s_1920x1080.mp4")
            video_paths = [default_video, default_video]

        config = RendererConfig()
        config.video_paths = video_paths
        config.hwnd = int(self.winId())
        config.width = self.width()
        config.height = self.height()
        config.use_hardware_decode = True

        if not self.renderer.initialize(config):
            print("Failed to initialize renderer")
            return False

        dur = self.renderer.duration_us() / 1_000_000
        print(f"Tracks: {self.renderer.track_count()}, Duration: {dur:.1f}s")
        print("Shortcuts: Space=Play/Pause | Left/Right=\u00b11frame | Shift+Left/Right=\u00b11s | M=Mode")
        print("Mouse: LeftDrag=Pan | Wheel=Zoom | RightDrag=SplitDivider")
        return True

    def _pts_s(self):
        return self.renderer.current_pts_us() / 1_000_000

    def _dur_s(self):
        return self.renderer.duration_us() / 1_000_000

    def _sync_layout(self):
        """Apply current layout state atomically."""
        self.renderer.apply_layout(self.layout)

    def _update_overlay(self):
        if self.renderer.is_initialized():
            pts = self._pts_s()
            dur = self._dur_s()
            state = "PLAY" if self.playing else "PAUSE"
            mode = MODE_NAMES.get(self.layout.mode, "?")
            zoom = f"{self.layout.zoom_ratio:.1f}x"
            self.setTitle(f"VoidPlayer  [{state}]  {pts:.3f}s / {dur:.1f}s  |  {mode}  Zoom:{zoom}")

    def keyPressEvent(self, event: QKeyEvent):
        key = event.key()
        shift = bool(event.modifiers() & Qt.KeyboardModifier.ShiftModifier)

        if key == Qt.Key.Key_Space:
            if self.playing:
                self.renderer.pause()
                self.playing = False
                print(f"  Pause  @ {self._pts_s():.3f}s")
            else:
                self.renderer.resume()
                self.playing = True
                print(f"  Play   @ {self._pts_s():.3f}s")

        elif key == Qt.Key.Key_Right:
            if shift:
                target = self.renderer.current_pts_us() + 1_000_000
                dur = self.renderer.duration_us()
                if target > dur:
                    target = dur - 100_000
                self.renderer.pause()
                self.renderer.seek(target, SeekType.Keyframe)
                self.playing = False
                print(f"  +1s    \u2192 {self._pts_s():.3f}s")
            else:
                self.renderer.step_forward()
                self.playing = False
                print(f"  Fwd+1  \u2192 {self._pts_s():.3f}s")

        elif key == Qt.Key.Key_Left:
            if shift:
                target = self.renderer.current_pts_us() - 1_000_000
                if target < 0:
                    target = 0
                self.renderer.pause()
                self.renderer.seek(target, SeekType.Keyframe)
                self.playing = False
                print(f"  -1s    \u2192 {self._pts_s():.3f}s")
            else:
                self.renderer.step_backward()
                self.playing = False
                print(f"  Back-1 \u2192 {self._pts_s():.3f}s")

        elif key == Qt.Key.Key_M:
            self.layout.mode = (
                LAYOUT_SPLIT_SCREEN if self.layout.mode == LAYOUT_SIDE_BY_SIDE
                else LAYOUT_SIDE_BY_SIDE
            )
            self.layout.zoom_ratio = 1.0
            self.layout.view_offset = [0.0, 0.0]
            self._sync_layout()
            mode = MODE_NAMES.get(self.layout.mode, "?")
            print(f"  Mode   \u2192 {mode}")

        else:
            super().keyPressEvent(event)

    def mousePressEvent(self, event: QMouseEvent):
        if event.button() == Qt.MouseButton.LeftButton:
            self._panning = True
            self._last_mouse_pos = event.position().toPoint()
        elif event.button() == Qt.MouseButton.RightButton:
            self._splitting = True
            self._last_mouse_pos = event.position().toPoint()

    def mouseReleaseEvent(self, event: QMouseEvent):
        if event.button() == Qt.MouseButton.LeftButton:
            self._panning = False
        elif event.button() == Qt.MouseButton.RightButton:
            self._splitting = False

    def mouseMoveEvent(self, event: QMouseEvent):
        pos = event.position().toPoint()
        delta = pos - self._last_mouse_pos

        if self._panning:
            sensitivity = 1.0 / max(self.layout.zoom_ratio, 1.0)
            vx, vy = self.layout.view_offset
            self.layout.view_offset = [
                vx + delta.x() * sensitivity,
                vy + delta.y() * sensitivity,
            ]
            self._sync_layout()

        if self._splitting and self.layout.mode == LAYOUT_SPLIT_SCREEN:
            self.layout.split_pos = pos.x() / max(self.width(), 1)
            self._sync_layout()

        self._last_mouse_pos = pos

    def wheelEvent(self, event: QWheelEvent):
        delta = event.angleDelta().y()
        if delta > 0:
            self.layout.zoom_ratio = min(self.layout.zoom_ratio * 1.1, 10.0)
        elif delta < 0:
            self.layout.zoom_ratio = max(self.layout.zoom_ratio / 1.1, 1.0)
        self._sync_layout()

    def cleanup(self):
        self.overlay_timer.stop()
        self.renderer.shutdown()


def main():
    video_paths = sys.argv[1:] if len(sys.argv) > 1 else None

    app = QApplication(sys.argv)

    window = VideoWindow()
    window.show()
    app.processEvents()

    if not window.init_renderer(video_paths):
        return 1

    window._sync_layout()
    window.renderer.play()
    window.playing = True
    window.overlay_timer.start()

    ret = app.exec()
    window.cleanup()
    return ret


if __name__ == "__main__":
    sys.exit(main())
