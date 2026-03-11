#!/usr/bin/env python3
"""
Multi-video playback demo using SyncController and PlaybackManager.

Demonstrates synchronized playback of multiple video sources with
configurable time offsets.
"""

import sys
import os
from pathlib import Path

# Project root is parent of tests directory
PROJECT_ROOT = Path(__file__).parent.parent

# Add FFmpeg DLLs to PATH (Windows) - MUST be done before importing voidview_native
ffmpeg_bin = PROJECT_ROOT / "libs" / "ffmpeg" / "bin"
if ffmpeg_bin.exists():
    if sys.platform == 'win32' and hasattr(os, 'add_dll_directory'):
        os.add_dll_directory(str(ffmpeg_bin))
    os.environ["PATH"] = str(ffmpeg_bin) + os.pathsep + os.environ.get("PATH", "")

# Add player module to path
sys.path.insert(0, str(PROJECT_ROOT / "player"))

print("Imports...", flush=True)

from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QPushButton, QLabel, QSlider, QSpinBox, QGroupBox, QSplitter
)
from PySide6.QtCore import Qt, QTimer
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtGui import QSurfaceFormat
from OpenGL.GL import *

from playback_manager import PlaybackManager, PlayState

print("Imports OK", flush=True)


class MultiVideoGLWidget(QOpenGLWidget):
    """OpenGL widget that renders multiple video textures side by side."""

    def __init__(self, playback_manager: PlaybackManager, parent=None):
        super().__init__(parent)
        self.playback_manager = playback_manager
        self.playback_manager.set_make_current_callback(self._make_gl_current)

        # View mode: "side_by_side" or "split_screen"
        self.view_mode = "side_by_side"

        # Track which textures to render
        self._needs_update = False

    def _make_gl_current(self):
        """Callback to make GL context current."""
        self.makeCurrent()

    def initializeGL(self):
        print("MultiVideoGLWidget initializeGL", flush=True)

        gl_version = glGetString(GL_VERSION)
        glsl_version = glGetString(GL_SHADING_LANGUAGE_VERSION)
        print(f"OpenGL: {gl_version.decode() if gl_version else 'N/A'}", flush=True)
        print(f"GLSL: {glsl_version.decode() if glsl_version else 'N/A'}", flush=True)

        glClearColor(0.1, 0.1, 0.1, 1.0)
        print("initializeGL complete", flush=True)

    def paintGL(self):
        glClear(GL_COLOR_BUFFER_BIT)

        sources = self.playback_manager.sources
        if not sources:
            return

        if self.view_mode == "side_by_side":
            self._render_side_by_side(sources)
        else:
            self._render_split_screen(sources)

    def _render_side_by_side(self, sources):
        """Render videos side by side."""
        count = len(sources)
        if count == 0:
            return

        # Calculate width for each video
        width_per_video = 2.0 / count

        glEnable(GL_TEXTURE_2D)

        for i, source in enumerate(sources):
            if source.texture_id == 0:
                continue

            # Calculate position
            x_offset = -1.0 + i * width_per_video

            # Set viewport for this video
            glViewport(
                int(x_offset * self.width() / 2 + self.width() / 2),
                0,
                int(self.width() / count),
                self.height()
            )

            # Bind texture
            glBindTexture(GL_TEXTURE_2D, source.texture_id)

            # Draw quad
            glColor4f(1, 1, 1, 1)
            glMatrixMode(GL_PROJECTION)
            glLoadIdentity()
            glMatrixMode(GL_MODELVIEW)
            glLoadIdentity()

            glBegin(GL_QUADS)
            glTexCoord2f(0, 1); glVertex2f(-1, -1)
            glTexCoord2f(1, 1); glVertex2f( 1, -1)
            glTexCoord2f(1, 0); glVertex2f( 1,  1)
            glTexCoord2f(0, 0); glVertex2f(-1,  1)
            glEnd()

        glDisable(GL_TEXTURE_2D)

        # Reset viewport
        glViewport(0, 0, self.width(), self.height())

    def _render_split_screen(self, sources):
        """Render videos with horizontal split."""
        count = len(sources)
        if count == 0:
            return

        height_per_video = 2.0 / count

        glEnable(GL_TEXTURE_2D)

        for i, source in enumerate(sources):
            if source.texture_id == 0:
                continue

            y_offset = 1.0 - (i + 1) * height_per_video

            glViewport(
                0,
                int((y_offset + 1) * self.height() / 2),
                self.width(),
                int(self.height() / count)
            )

            glBindTexture(GL_TEXTURE_2D, source.texture_id)

            glColor4f(1, 1, 1, 1)
            glMatrixMode(GL_PROJECTION)
            glLoadIdentity()
            glMatrixMode(GL_MODELVIEW)
            glLoadIdentity()

            glBegin(GL_QUADS)
            glTexCoord2f(0, 1); glVertex2f(-1, -1)
            glTexCoord2f(1, 1); glVertex2f( 1, -1)
            glTexCoord2f(1, 0); glVertex2f( 1,  1)
            glTexCoord2f(0, 0); glVertex2f(-1,  1)
            glEnd()

        glDisable(GL_TEXTURE_2D)
        glViewport(0, 0, self.width(), self.height())

    def request_update(self):
        """Request a repaint."""
        self.update()

    def set_view_mode(self, mode: str):
        """Set view mode: 'side_by_side' or 'split_screen'."""
        self.view_mode = mode
        self.update()


class MainWindow(QMainWindow):
    """Main window for multi-video playback demo."""

    def __init__(self, video_paths: list[str]):
        super().__init__()

        self.setWindowTitle("VoidPlayer - Multi-Video Demo")
        self.resize(1200, 800)

        # Create playback manager
        self.playback_manager = PlaybackManager(self)

        # Connect signals
        self.playback_manager.frame_ready.connect(self._on_frame_ready)
        self.playback_manager.time_changed.connect(self._on_time_changed)
        self.playback_manager.state_changed.connect(self._on_state_changed)
        self.playback_manager.source_loaded.connect(self._on_source_loaded)
        self.playback_manager.source_error.connect(self._on_source_error)
        self.playback_manager.eof_reached.connect(self._on_eof_reached)

        # Build UI
        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)

        # GL widget
        self.gl_widget = MultiVideoGLWidget(self.playback_manager)
        layout.addWidget(self.gl_widget, stretch=1)

        # Time display and slider
        time_layout = QHBoxLayout()

        self.time_label = QLabel("00:00.000")
        self.time_label.setMinimumWidth(100)
        time_layout.addWidget(self.time_label)

        self.time_slider = QSlider(Qt.Horizontal)
        self.time_slider.setRange(0, 1000)
        self.time_slider.setValue(0)
        self.time_slider.sliderPressed.connect(self._on_slider_pressed)
        self.time_slider.sliderReleased.connect(self._on_slider_released)
        self.time_slider.valueChanged.connect(self._on_slider_moved)
        time_layout.addWidget(self.time_slider, stretch=1)

        self.duration_label = QLabel("/ 00:00.000")
        self.duration_label.setMinimumWidth(100)
        time_layout.addWidget(self.duration_label)

        layout.addLayout(time_layout)

        # Control buttons
        btn_layout = QHBoxLayout()

        self.play_btn = QPushButton("Play")
        self.play_btn.clicked.connect(self._toggle_play)
        btn_layout.addWidget(self.play_btn)

        self.pause_btn = QPushButton("Pause")
        self.pause_btn.clicked.connect(self.playback_manager.pause)
        btn_layout.addWidget(self.pause_btn)

        self.stop_btn = QPushButton("Stop")
        self.stop_btn.clicked.connect(self.playback_manager.stop)
        btn_layout.addWidget(self.stop_btn)

        self.step_btn = QPushButton("Step Frame")
        self.step_btn.clicked.connect(self._step_frame)
        btn_layout.addWidget(self.step_btn)

        layout.addLayout(btn_layout)

        # Offset controls
        self.offset_group = QGroupBox("Time Offsets (ms)")
        offset_layout = QHBoxLayout(self.offset_group)
        self.offset_spinboxes = []
        layout.addWidget(self.offset_group)

        # Status label
        self.status_label = QLabel("Ready")
        layout.addWidget(self.status_label)

        # Track slider state
        self._slider_seeking = False

        # Load videos after GL context is ready
        self._video_paths = video_paths
        self._loaded_count = 0

        # Defer loading until GL widget is initialized
        QTimer.singleShot(100, self._load_videos)

    def _load_videos(self):
        """Load all video sources."""
        self.status_label.setText(f"Loading {len(self._video_paths)} videos...")

        # Create source list
        sources = [(i, path) for i, path in enumerate(self._video_paths)]

        # Make GL context current before loading
        self.gl_widget.makeCurrent()

        if self.playback_manager.load_sources(sources):
            self.status_label.setText(f"Loaded {len(self._video_paths)} videos")
            self._update_duration_label()
            self._create_offset_controls()
        else:
            self.status_label.setText("Failed to load videos")

    def _create_offset_controls(self):
        """Create offset spinboxes for each source."""
        # Clear existing
        for spinbox in self.offset_spinboxes:
            spinbox.deleteLater()
        self.offset_spinboxes.clear()

        # Create new spinboxes
        for i, source in enumerate(self.playback_manager.sources):
            label = QLabel(f"Source {i}:")
            self.offset_group.layout().addWidget(label)

            spinbox = QSpinBox()
            spinbox.setRange(-10000, 10000)
            spinbox.setValue(0)
            spinbox.setSingleStep(100)
            spinbox.valueChanged.connect(lambda v, idx=i: self._on_offset_changed(idx, v))
            self.offset_spinboxes.append(spinbox)
            self.offset_group.layout().addWidget(spinbox)

    def _on_offset_changed(self, index: int, value: int):
        """Handle offset spinbox change."""
        self.playback_manager.set_offset(index, value)
        self.status_label.setText(f"Source {index} offset: {value}ms")

    def _on_frame_ready(self, results: dict):
        """Handle new frame decoded."""
        self.gl_widget.request_update()

    def _on_time_changed(self, time_ms: int):
        """Handle time change."""
        self._update_time_label(time_ms)

        if not self._slider_seeking:
            duration = self.playback_manager.duration_ms
            if duration > 0:
                pos = int(time_ms / duration * 1000)
                self.time_slider.blockSignals(True)
                self.time_slider.setValue(pos)
                self.time_slider.blockSignals(False)

    def _on_state_changed(self, state_value: int):
        """Handle state change."""
        # Convert int value back to PlayState
        state = PlayState(state_value)
        state_names = {
            PlayState.STOPPED: "Stopped",
            PlayState.PLAYING: "Playing",
            PlayState.PAUSED: "Paused"
        }
        self.status_label.setText(state_names.get(state, "Unknown"))
        self.play_btn.setText("Play" if state != PlayState.PLAYING else "Resume")

    def _on_source_loaded(self, index: int, path: str):
        """Handle source loaded."""
        print(f"Source {index} loaded: {path}", flush=True)
        self._loaded_count += 1

    def _on_source_error(self, index: int, error: str):
        """Handle source error."""
        print(f"Source {index} error: {error}", flush=True)
        self.status_label.setText(f"Source {index} error: {error}")

    def _on_eof_reached(self):
        """Handle EOF reached."""
        self.status_label.setText("Playback complete (EOF)")

    def _on_slider_pressed(self):
        """Handle slider pressed."""
        self._slider_seeking = True

    def _on_slider_released(self):
        """Handle slider released."""
        self._slider_seeking = False
        value = self.time_slider.value()
        duration = self.playback_manager.duration_ms
        if duration > 0:
            target_ms = int(value / 1000 * duration)
            self.playback_manager.seek_to(target_ms)

    def _on_slider_moved(self, value: int):
        """Handle slider moved while seeking."""
        if self._slider_seeking:
            duration = self.playback_manager.duration_ms
            if duration > 0:
                time_ms = int(value / 1000 * duration)
                self._update_time_label(time_ms)

    def _update_time_label(self, time_ms: int):
        """Update time display."""
        seconds = time_ms // 1000
        minutes = seconds // 60
        secs = seconds % 60
        millis = time_ms % 1000
        self.time_label.setText(f"{minutes:02d}:{secs:02d}.{millis:03d}")

    def _update_duration_label(self):
        """Update duration display."""
        duration = self.playback_manager.duration_ms
        seconds = duration // 1000
        minutes = seconds // 60
        secs = seconds % 60
        millis = duration % 1000
        self.duration_label.setText(f"/ {minutes:02d}:{secs:02d}.{millis:03d}")

    def _toggle_play(self):
        """Toggle play/pause."""
        self.playback_manager.toggle_play_pause()

    def _step_frame(self):
        """Step one frame."""
        self.playback_manager.pause()
        results = self.playback_manager.step_frame()
        self.gl_widget.request_update()
        self.status_label.setText(f"Stepped: {results}")


def main():
    # Use the same video multiple times for demo
    video = str(PROJECT_ROOT / "resources" / "video" / "TheaterSquare_1920x1080.mp4")

    # For demo: load the same video 3 times with different offsets
    # In real use, these would be different video files
    videos = [video, video, video]

    fmt = QSurfaceFormat()
    fmt.setVersion(2, 1)  # Compatibility profile
    QSurfaceFormat.setDefaultFormat(fmt)

    app = QApplication(sys.argv)
    print("QApplication ready", flush=True)

    win = MainWindow(videos)
    win.show()
    print("Window shown, entering loop...", flush=True)

    ret = app.exec()
    print(f"Exit: {ret}", flush=True)
    return ret


if __name__ == "__main__":
    sys.exit(main())
