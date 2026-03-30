"""Interactive demo: Multi-track video renderer with keyboard controls.

Keyboard shortcuts:
  Space        Play / Pause
  Right        Step forward 1 frame
  Left         Step backward 1 frame
  Shift+Right  Seek forward 1 second
  Shift+Left   Seek backward 1 second
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
from PySide6.QtGui import QWindow, QKeyEvent
from PySide6.QtCore import Qt, QTimer
from video_renderer_native import Renderer, RendererConfig, SeekType

VIDEO_DIR = Path(__file__).resolve().parent.parent.parent / "resources" / "video"


class VideoWindow(QWindow):
    def __init__(self):
        super().__init__()
        self.setTitle("VoidPlayer - Keyboard Demo  [Space]Play/Pause  [Left/Right]Frame  [Shift+Left/Right]1s")
        self.resize(1280, 720)

        self.renderer = Renderer()
        self.playing = False

        # PTS overlay refresh
        self.overlay_timer = QTimer(self)
        self.overlay_timer.setInterval(100)
        self.overlay_timer.timeout.connect(self._update_overlay)

    def init_renderer(self, video_path: str | None = None):
        config = RendererConfig()
        config.video_paths = [
            video_path or str(VIDEO_DIR / "h265_10s_1920x1080.mp4"),
        ]
        config.hwnd = int(self.winId())
        config.width = self.width()
        config.height = self.height()
        config.use_hardware_decode = True

        if not self.renderer.initialize(config):
            print("Failed to initialize renderer")
            return False

        dur = self.renderer.duration_us() / 1_000_000
        print(f"Tracks: {self.renderer.track_count()}, Duration: {dur:.1f}s")
        print("Shortcuts: Space=Play/Pause | Left/Right=±1frame | Shift+Left/Right=±1s")
        return True

    def _pts_s(self):
        return self.renderer.current_pts_us() / 1_000_000

    def _dur_s(self):
        return self.renderer.duration_us() / 1_000_000

    def _update_overlay(self):
        if self.renderer.is_initialized():
            pts = self._pts_s()
            dur = self._dur_s()
            state = "PLAY" if self.playing else "PAUSE"
            self.setTitle(f"VoidPlayer  [{state}]  {pts:.3f}s / {dur:.1f}s")

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
                # Seek forward 1 second
                target = self.renderer.current_pts_us() + 1_000_000
                dur = self.renderer.duration_us()
                if target > dur:
                    target = dur - 100_000
                self.renderer.pause()
                self.renderer.seek(target, SeekType.Keyframe)
                self.playing = False
                print(f"  +1s    → {self._pts_s():.3f}s")
            else:
                # Step forward 1 frame
                self.renderer.step_forward()
                self.playing = False
                print(f"  Fwd+1  → {self._pts_s():.3f}s")

        elif key == Qt.Key.Key_Left:
            if shift:
                # Seek backward 1 second
                target = self.renderer.current_pts_us() - 1_000_000
                if target < 0:
                    target = 0
                self.renderer.pause()
                self.renderer.seek(target, SeekType.Keyframe)
                self.playing = False
                print(f"  -1s    → {self._pts_s():.3f}s")
            else:
                # Step backward 1 frame
                self.renderer.step_backward()
                self.playing = False
                print(f"  Back-1 → {self._pts_s():.3f}s")

        else:
            super().keyPressEvent(event)

    def cleanup(self):
        self.overlay_timer.stop()
        self.renderer.shutdown()


def main():
    video_path = sys.argv[1] if len(sys.argv) > 1 else None

    app = QApplication(sys.argv)

    window = VideoWindow()
    window.show()
    app.processEvents()

    if not window.init_renderer(video_path):
        return 1

    window.renderer.play()
    window.playing = True
    window.overlay_timer.start()

    ret = app.exec()
    window.cleanup()
    return ret


if __name__ == "__main__":
    sys.exit(main())
