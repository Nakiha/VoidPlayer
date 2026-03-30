"""Seek functionality demo: play, seek back/forward, frame step."""
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
from PySide6.QtCore import QTimer
from video_renderer_native import Renderer, RendererConfig, SeekType

VIDEO_DIR = Path(__file__).resolve().parent.parent.parent / "resources" / "video"


class SeekDemo:
    PHASE_PLAY_3S = 0
    PHASE_SEEK_BACK_2S = 1
    PHASE_STEP_BACK_1 = 2
    PHASE_STEP_FWD_1 = 3
    PHASE_PLAY_2S = 4
    PHASE_SEEK_FWD_2S = 5
    PHASE_DONE = 6

    PHASE_NAMES = {
        PHASE_PLAY_3S: "Play 3s",
        PHASE_SEEK_BACK_2S: "Seek back 2s",
        PHASE_STEP_BACK_1: "Step back 1 frame",
        PHASE_STEP_FWD_1: "Step fwd 1 frame",
        PHASE_PLAY_2S: "Play 2s",
        PHASE_SEEK_FWD_2S: "Seek forward 2s",
        PHASE_DONE: "Done",
    }

    def __init__(self, renderer: Renderer):
        self.renderer = renderer
        self.phase = self.PHASE_PLAY_3S
        self.timer = QTimer()
        self.timer.setSingleShot(True)
        self.timer.timeout.connect(self.next_step)

    def pts_s(self):
        return self.renderer.current_pts_us() / 1_000_000

    def start(self):
        print(f"\n=== Seek Demo ===")
        print(f"Duration: {self.renderer.duration_us() / 1_000_000:.1f}s")
        self.renderer.play()
        self._advance("Play 3 seconds", 3000)

    def _advance(self, msg, delay_ms):
        print(f"  PTS={self.pts_s():.3f}s")
        self.timer.start(delay_ms)

    def next_step(self):
        pts = self.pts_s()

        if self.phase == self.PHASE_PLAY_3S:
            print(f"\n[Phase] Seek back 2s (current: {pts:.3f}s)")
            self.renderer.pause()
            target = max(0, self.renderer.current_pts_us() - 2_000_000)
            self.renderer.seek(target, SeekType.Keyframe)
            self.phase = self.PHASE_SEEK_BACK_2S
            QTimer.singleShot(500, self.next_step)
            return

        elif self.phase == self.PHASE_SEEK_BACK_2S:
            print(f"  -> After seek back: PTS={pts:.3f}s")
            print(f"\n[Phase] Step back 1 frame")
            self.renderer.step_backward()
            print(f"  -> After step_back: PTS={self.pts_s():.3f}s")
            self.phase = self.PHASE_STEP_BACK_1
            self.timer.start(1000)

        elif self.phase == self.PHASE_STEP_BACK_1:
            print(f"\n[Phase] Step forward 1 frame")
            self.renderer.step_forward()
            print(f"  -> After step_fwd: PTS={self.pts_s():.3f}s")
            self.phase = self.PHASE_STEP_FWD_1
            self.timer.start(1000)

        elif self.phase == self.PHASE_STEP_FWD_1:
            print(f"\n[Phase] Play 2 seconds")
            self.renderer.resume()
            self.phase = self.PHASE_PLAY_2S
            self.timer.start(2000)

        elif self.phase == self.PHASE_PLAY_2S:
            print(f"\n[Phase] Seek forward 2s (current: {pts:.3f}s)")
            self.renderer.pause()
            target = self.renderer.current_pts_us() + 2_000_000
            duration = self.renderer.duration_us()
            if target > duration:
                target = duration - 500_000
            self.renderer.seek(target, SeekType.Keyframe)
            self.phase = self.PHASE_SEEK_FWD_2S
            QTimer.singleShot(500, self.next_step)
            return

        elif self.phase == self.PHASE_SEEK_FWD_2S:
            print(f"  -> After seek fwd: PTS={pts:.3f}s")
            print(f"\n=== Demo complete ===")
            self.phase = self.PHASE_DONE
            # Resume briefly then shutdown
            self.renderer.resume()
            self.timer.start(1000)

        elif self.phase == self.PHASE_DONE:
            QApplication.quit()


def main():
    app = QApplication(sys.argv)

    window = QWindow()
    window.setTitle("VoidPlayer - Seek Demo")
    window.resize(1280, 720)
    window.show()
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

    demo = SeekDemo(renderer)
    demo.start()

    ret = app.exec()
    renderer.shutdown()
    return ret


if __name__ == "__main__":
    sys.exit(main())
