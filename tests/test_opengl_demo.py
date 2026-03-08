#!/usr/bin/env python3
"""
Minimal OpenGL demo for voidview_native decoder
"""

import sys
import os
from pathlib import Path

# Project root is parent of tests directory
PROJECT_ROOT = Path(__file__).parent.parent

# Add FFmpeg DLLs to PATH (Windows) - MUST be done before importing voidview_native
ffmpeg_bin = PROJECT_ROOT / "libs" / "ffmpeg" / "bin"
if ffmpeg_bin.exists():
    # Use add_dll_directory for Python 3.8+ on Windows
    if sys.platform == 'win32' and hasattr(os, 'add_dll_directory'):
        os.add_dll_directory(str(ffmpeg_bin))
    # Also update PATH as fallback
    os.environ["PATH"] = str(ffmpeg_bin) + os.pathsep + os.environ.get("PATH", "")

# Add player module to path
sys.path.insert(0, str(PROJECT_ROOT / "player"))

print("Imports...", flush=True)

from PySide6.QtWidgets import QApplication, QMainWindow, QVBoxLayout, QWidget, QPushButton, QLabel
from PySide6.QtCore import QTimer
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtGui import QSurfaceFormat
from OpenGL.GL import *
import voidview_native

print("Imports OK", flush=True)


class SimpleGLWidget(QOpenGLWidget):
    def __init__(self, video_path, parent=None):
        super().__init__(parent)
        self.video_path = video_path
        self.decoder = None
        self.texture_id = 0
        self.frame_count = 0

    def initializeGL(self):
        print("initializeGL called", flush=True)

        gl_version = glGetString(GL_VERSION)
        glsl_version = glGetString(GL_SHADING_LANGUAGE_VERSION)
        print(f"OpenGL: {gl_version.decode() if gl_version else 'N/A'}", flush=True)
        print(f"GLSL: {glsl_version.decode() if glsl_version else 'N/A'}", flush=True)

        # Create decoder
        print("Creating decoder...", flush=True)
        self.decoder = voidview_native.HardwareDecoder(self.video_path)

        # Initialize
        print("Initializing decoder...", flush=True)
        if not self.decoder.initialize(0):
            print(f"Init failed: {self.decoder.error_message}", flush=True)
            return

        print(f"Video: {self.decoder.width}x{self.decoder.height}, {self.decoder.duration_ms}ms", flush=True)

        # Set OpenGL context for texture sharing
        print("Setting GL context...", flush=True)
        self.decoder.set_opengl_context(0)
        print("set_opengl_context done", flush=True)

        glClearColor(0.0, 0.0, 0.0, 1.0)
        print("initializeGL complete", flush=True)

    def paintGL(self):
        glClear(GL_COLOR_BUFFER_BIT)

        if not self.texture_id:
            return

        # Enable texturing
        glEnable(GL_TEXTURE_2D)
        glBindTexture(GL_TEXTURE_2D, self.texture_id)

        # Set up proper 2D projection
        glMatrixMode(GL_PROJECTION)
        glLoadIdentity()
        glMatrixMode(GL_MODELVIEW)
        glLoadIdentity()

        # Draw textured quad (full screen)
        glColor4f(1, 1, 1, 1)
        glBegin(GL_QUADS)
        glTexCoord2f(0, 1); glVertex2f(-1, -1)  # Bottom-left (flip Y)
        glTexCoord2f(1, 1); glVertex2f( 1, -1)  # Bottom-right
        glTexCoord2f(1, 0); glVertex2f( 1,  1)  # Top-right
        glTexCoord2f(0, 0); glVertex2f(-1,  1)  # Top-left
        glEnd()

        glDisable(GL_TEXTURE_2D)

    def decode_frame(self):
        if not self.decoder:
            return False

        # Ensure OpenGL context is current
        self.makeCurrent()

        if self.decoder.decode_next_frame():
            self.texture_id = self.decoder.texture_id
            self.frame_count += 1
            print(f"Frame {self.frame_count}: tex={self.texture_id}, pts={self.decoder.current_pts_ms}ms", flush=True)
            return True
        elif self.decoder.eof:
            print("EOF", flush=True)
            return False
        elif self.decoder.has_error:
            print(f"Error: {self.decoder.error_message}", flush=True)
            return False
        return False


class MainWindow(QMainWindow):
    def __init__(self, video_path):
        super().__init__()

        self.setWindowTitle("VoidPlayer - OpenGL Demo")
        self.resize(800, 600)

        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)

        self.gl_widget = SimpleGLWidget(video_path)
        layout.addWidget(self.gl_widget, stretch=1)

        self.label = QLabel("Ready")
        layout.addWidget(self.label)

        btn = QPushButton("Step Frame")
        btn.clicked.connect(self.step_frame)
        layout.addWidget(btn)

        self.update_timer = QTimer(self)
        self.update_timer.timeout.connect(self.update_label)
        self.update_timer.start(100)

        # Auto-play timer - decode frames at ~30fps
        self.play_timer = QTimer(self)
        self.play_timer.timeout.connect(self.auto_step)
        self.play_timer.start(33)  # ~30fps

    def step_frame(self):
        self.gl_widget.decode_frame()
        self.gl_widget.update()

    def auto_step(self):
        if self.gl_widget.decoder and not self.gl_widget.decoder.eof:
            self.step_frame()

    def update_label(self):
        if self.gl_widget.decoder:
            self.label.setText(
                f"Frame: {self.gl_widget.frame_count} | "
                f"PTS: {self.gl_widget.decoder.current_pts_ms}ms | "
                f"Tex: {self.gl_widget.texture_id}"
            )


def main():
    video = str(PROJECT_ROOT / "resources" / "video" / "TheaterSquare_1920x1080.mp4")

    fmt = QSurfaceFormat()
    fmt.setVersion(2, 1)  # Compatibility profile
    QSurfaceFormat.setDefaultFormat(fmt)

    app = QApplication(sys.argv)
    print("QApplication ready", flush=True)

    win = MainWindow(video)
    win.show()
    print("Window shown, entering loop...", flush=True)

    ret = app.exec()
    print(f"Exit: {ret}", flush=True)
    return ret


if __name__ == "__main__":
    sys.exit(main())
