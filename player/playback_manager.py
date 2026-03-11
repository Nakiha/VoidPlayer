"""
PlaybackManager - Playback state and decoder coordination

Coordinates SyncController with Qt signals for UI integration.
"""

from __future__ import annotations
from enum import Enum, auto
from typing import Optional, Callable
from pathlib import Path

from PySide6.QtCore import QObject, Signal, QTimer

import voidview_native

# Support both relative and absolute imports
try:
    from .sync_controller import SyncController, VideoSource
except ImportError:
    from sync_controller import SyncController, VideoSource


class PlayState(Enum):
    """Playback state enumeration."""
    STOPPED = auto()
    PLAYING = auto()
    PAUSED = auto()


class PlaybackManager(QObject):
    """
    Manages playback state and coordinates SyncController with decoders.

    Signals:
        frame_ready: Emitted when new frames are decoded (passes dict of source indices)
        time_changed: Emitted when master clock updates (passes current time in ms)
        duration_changed: Emitted when sources are loaded (passes duration in ms)
        state_changed: Emitted when playback state changes (passes PlayState)
        source_loaded: Emitted when a source loads successfully (passes index, path)
        source_error: Emitted when a source fails to load (passes index, error message)
        eof_reached: Emitted when all sources reach EOF
    """

    # Signals
    frame_ready = Signal(object)  # {source_index: has_new_frame}
    time_changed = Signal(int)  # current_time_ms
    duration_changed = Signal(int)  # duration_ms
    state_changed = Signal(int)  # PlayState enum value
    source_loaded = Signal(int, str)  # index, path
    source_error = Signal(int, str)  # index, error_message
    eof_reached = Signal()

    def __init__(self, parent: Optional[QObject] = None):
        super().__init__(parent)

        self._sync_controller = SyncController()
        self._state = PlayState.STOPPED
        self._tick_timer = QTimer(self)
        self._tick_timer.timeout.connect(self._on_tick)
        self._tick_interval_ms = 16  # ~60fps

        # OpenGL context callback - set by GL widget
        self._make_current_callback: Optional[Callable[[], None]] = None

        # Track loaded sources
        self._source_paths: dict[int, str] = {}

    @property
    def sync_controller(self) -> SyncController:
        """Access the underlying SyncController."""
        return self._sync_controller

    @property
    def state(self) -> PlayState:
        """Current playback state."""
        return self._state

    @property
    def current_time_ms(self) -> int:
        """Current master clock time."""
        return self._sync_controller.current_time_ms

    @property
    def duration_ms(self) -> int:
        """Total duration."""
        return self._sync_controller.duration_ms

    @property
    def is_playing(self) -> bool:
        """Whether currently playing."""
        return self._state == PlayState.PLAYING

    @property
    def sources(self) -> list[VideoSource]:
        """List of all video sources."""
        return self._sync_controller.sources

    def set_make_current_callback(self, callback: Optional[Callable[[], None]]) -> None:
        """
        Set callback to make OpenGL context current before decoding.

        Must be called before loading sources or playing.
        """
        self._make_current_callback = callback

    def _ensure_gl_context(self) -> bool:
        """Ensure OpenGL context is current."""
        if self._make_current_callback:
            self._make_current_callback()
            return True
        return False

    def load_source(self, index: int, source_path: str, hw_device_type: int = 0) -> bool:
        """
        Load a single video source.

        Args:
            index: Unique source index
            source_path: Path to video file
            hw_device_type: Hardware device type (0=Auto, 1=D3D11VA, 2=NVDEC)

        Returns:
            True if loaded successfully
        """
        self._ensure_gl_context()

        # Check file exists
        path = Path(source_path)
        if not path.exists():
            self.source_error.emit(index, f"File not found: {source_path}")
            return False

        try:
            # Create decoder
            decoder = voidview_native.HardwareDecoder(str(path.absolute()))

            # Initialize
            if not decoder.initialize(hw_device_type):
                error_msg = decoder.error_message or "Initialization failed"
                self.source_error.emit(index, error_msg)
                return False

            # Set OpenGL context for texture sharing
            decoder.set_opengl_context(0)

            # Add to sync controller
            self._sync_controller.add_source(index, decoder)
            self._source_paths[index] = str(path.absolute())

            # Emit signals
            self.source_loaded.emit(index, str(path.absolute()))
            self.duration_changed.emit(self._sync_controller.duration_ms)

            return True

        except Exception as e:
            self.source_error.emit(index, str(e))
            return False

    def load_sources(self, sources: list[tuple[int, str]], hw_device_type: int = 0) -> bool:
        """
        Load multiple video sources.

        Args:
            sources: List of (index, path) tuples
            hw_device_type: Hardware device type

        Returns:
            True if all sources loaded successfully
        """
        all_success = True
        for index, path in sources:
            if not self.load_source(index, path, hw_device_type):
                all_success = False
        return all_success

    def unload_all(self) -> None:
        """Unload all sources."""
        self.stop()
        self._sync_controller = SyncController()
        self._source_paths.clear()

    def set_offset(self, index: int, offset_ms: int) -> bool:
        """Set time offset for a source."""
        return self._sync_controller.set_offset(index, offset_ms)

    def play(self) -> None:
        """Start or resume playback."""
        if self._state == PlayState.PLAYING:
            return

        # Check if all sources reached EOF
        if self._sync_controller.all_eof():
            # Restart from beginning
            self._sync_controller.stop()

        self._sync_controller.play()
        self._state = PlayState.PLAYING
        self._tick_timer.start(self._tick_interval_ms)
        self.state_changed.emit(self._state.value)

    def pause(self) -> None:
        """Pause playback."""
        if self._state != PlayState.PLAYING:
            return

        self._sync_controller.pause()
        self._tick_timer.stop()
        self._state = PlayState.PAUSED
        self.state_changed.emit(self._state.value)

    def stop(self) -> None:
        """Stop playback and reset to beginning."""
        self._tick_timer.stop()
        self._sync_controller.stop()
        self._state = PlayState.STOPPED
        self.state_changed.emit(self._state.value)
        self.time_changed.emit(0)

    def toggle_play_pause(self) -> None:
        """Toggle between play and pause."""
        if self._state == PlayState.PLAYING:
            self.pause()
        else:
            self.play()

    def seek_to(self, timestamp_ms: int) -> None:
        """Seek to a specific timestamp."""
        self._ensure_gl_context()
        self._sync_controller.seek_to(timestamp_ms)
        self.time_changed.emit(self._sync_controller.current_time_ms)

    def step_frame(self) -> dict[int, bool]:
        """
        Step forward one frame (for frame-by-frame playback).

        Returns:
            Dict of which sources decoded new frames
        """
        self._ensure_gl_context()

        results = {}
        for source in self._sync_controller.sources:
            if source.eof or source.has_error:
                results[source.index] = False
                continue

            if source.decoder.decode_next_frame():
                source.texture_id = source.decoder.texture_id
                source.last_pts_ms = source.decoder.current_pts_ms
                results[source.index] = True
            elif source.decoder.eof:
                source.eof = True
                results[source.index] = False
            elif source.decoder.has_error:
                source.has_error = True
                source.error_message = source.decoder.error_message
                results[source.index] = False

        if results:
            self.frame_ready.emit(results)

        return results

    def _on_tick(self) -> None:
        """Timer callback for continuous playback."""
        self._ensure_gl_context()

        # Update sync controller
        results = self._sync_controller.tick()

        # Emit time changed
        self.time_changed.emit(self._sync_controller.current_time_ms)

        # Emit frame ready if any new frames
        if any(results.values()):
            self.frame_ready.emit(results)

        # Check for EOF
        if self._sync_controller.all_eof():
            self._tick_timer.stop()
            self._state = PlayState.STOPPED
            self.state_changed.emit(self._state.value)
            self.eof_reached.emit()

    def get_source_texture(self, index: int) -> int:
        """Get the current texture ID for a source."""
        source = self._sync_controller.get_source(index)
        return source.texture_id if source else 0

    def get_source_info(self, index: int) -> Optional[dict]:
        """Get information about a source."""
        source = self._sync_controller.get_source(index)
        if not source:
            return None

        return {
            'index': source.index,
            'path': self._source_paths.get(index, ''),
            'offset_ms': source.offset_ms,
            'last_pts_ms': source.last_pts_ms,
            'texture_id': source.texture_id,
            'eof': source.eof,
            'has_error': source.has_error,
            'error_message': source.error_message,
            'width': source.decoder.width,
            'height': source.decoder.height,
            'duration_ms': source.decoder.duration_ms,
        }
