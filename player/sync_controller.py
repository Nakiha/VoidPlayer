"""
SyncController - Multi-video time synchronization

Manages multiple video sources with independent time offsets,
providing a unified master clock for synchronized playback.
"""

from __future__ import annotations
from dataclasses import dataclass, field
from typing import Optional, List
import time

import voidview_native


@dataclass
class VideoSource:
    """Represents a single video source with its decoder and sync settings."""
    index: int
    decoder: voidview_native.HardwareDecoder
    offset_ms: int = 0
    last_pts_ms: int = 0
    texture_id: int = 0
    has_error: bool = False
    error_message: str = ""
    eof: bool = False


class SyncController:
    """
    Multi-video time synchronization controller.

    Uses a virtual master clock to coordinate playback across
    multiple video sources with configurable time offsets.
    """

    def __init__(self):
        self._sources: dict[int, VideoSource] = {}
        self._master_clock_ms: int = 0
        self._start_time: Optional[float] = None
        self._is_playing: bool = False
        self._duration_ms: int = 0

    @property
    def current_time_ms(self) -> int:
        """Current master clock value in milliseconds."""
        if self._is_playing and self._start_time is not None:
            elapsed = int((time.perf_counter() - self._start_time) * 1000)
            return self._master_clock_ms + elapsed
        return self._master_clock_ms

    @property
    def duration_ms(self) -> int:
        """Duration of the longest video in milliseconds."""
        return self._duration_ms

    @property
    def is_playing(self) -> bool:
        """Whether playback is active."""
        return self._is_playing

    @property
    def sources(self) -> List[VideoSource]:
        """List of all video sources."""
        return list(self._sources.values())

    @property
    def source_count(self) -> int:
        """Number of video sources."""
        return len(self._sources)

    def add_source(self, index: int, decoder: voidview_native.HardwareDecoder) -> VideoSource:
        """
        Add a video source with the given index.

        Args:
            index: Unique identifier for this source
            decoder: Initialized HardwareDecoder instance

        Returns:
            The created VideoSource object
        """
        source = VideoSource(index=index, decoder=decoder)
        self._sources[index] = source

        # Update duration to be the maximum
        source_duration = decoder.duration_ms
        if source_duration > self._duration_ms:
            self._duration_ms = source_duration

        return source

    def remove_source(self, index: int) -> bool:
        """Remove a video source by index."""
        if index in self._sources:
            del self._sources[index]
            # Recalculate duration
            self._duration_ms = max(
                (s.decoder.duration_ms for s in self._sources.values()),
                default=0
            )
            return True
        return False

    def get_source(self, index: int) -> Optional[VideoSource]:
        """Get a video source by index."""
        return self._sources.get(index)

    def set_offset(self, index: int, offset_ms: int) -> bool:
        """
        Set time offset for a video source.

        Args:
            index: Source index
            offset_ms: Offset in milliseconds (positive = delayed, negative = advanced)

        Returns:
            True if offset was set successfully
        """
        source = self._sources.get(index)
        if source:
            source.offset_ms = offset_ms
            return True
        return False

    def play(self) -> None:
        """Start or resume playback."""
        if not self._is_playing:
            self._is_playing = True
            self._start_time = time.perf_counter()

    def pause(self) -> None:
        """Pause playback."""
        if self._is_playing:
            # Capture current time before pausing
            self._master_clock_ms = self.current_time_ms
            self._is_playing = False
            self._start_time = None

    def stop(self) -> None:
        """Stop playback and reset to beginning."""
        self._is_playing = False
        self._master_clock_ms = 0
        self._start_time = None

        # Reset all decoders
        for source in self._sources.values():
            source.last_pts_ms = 0
            source.eof = False
            source.has_error = False
            # Seek to beginning
            source.decoder.seek_to(0)

    def seek_to(self, timestamp_ms: int) -> None:
        """
        Seek all sources to the given master clock timestamp.

        Args:
            timestamp_ms: Target timestamp in milliseconds
        """
        was_playing = self._is_playing

        # Pause to update clock
        if was_playing:
            self.pause()

        self._master_clock_ms = max(0, min(timestamp_ms, self._duration_ms))

        # Seek each decoder to its target position
        for source in self._sources.values():
            target_pts = self._master_clock_ms + source.offset_ms
            if target_pts < 0:
                target_pts = 0
            elif target_pts > source.decoder.duration_ms:
                target_pts = source.decoder.duration_ms

            source.decoder.seek_to(target_pts)
            source.last_pts_ms = target_pts
            source.eof = False
            source.has_error = False

        # Resume if was playing
        if was_playing:
            self.play()

    def tick(self) -> dict[int, bool]:
        """
        Advance the clock and decode frames as needed.

        Should be called periodically (e.g., by a timer).

        Returns:
            Dict mapping source index to whether a new frame was decoded
        """
        if not self._is_playing:
            return {}

        results: dict[int, bool] = {}
        master_time = self.current_time_ms

        for index, source in self._sources.items():
            if source.eof or source.has_error:
                results[index] = False
                continue

            # Calculate target PTS for this source
            target_pts = master_time + source.offset_ms

            # Check if we need a new frame
            if source.last_pts_ms < target_pts:
                # Decode next frame
                if source.decoder.decode_next_frame():
                    source.texture_id = source.decoder.texture_id
                    source.last_pts_ms = source.decoder.current_pts_ms
                    results[index] = True
                elif source.decoder.eof:
                    source.eof = True
                    results[index] = False
                elif source.decoder.has_error:
                    source.has_error = True
                    source.error_message = source.decoder.error_message
                    results[index] = False
            else:
                results[index] = False

        return results

    def all_eof(self) -> bool:
        """Check if all sources have reached EOF."""
        if not self._sources:
            return True
        return all(s.eof or s.has_error for s in self._sources.values())

    def any_error(self) -> bool:
        """Check if any source has an error."""
        return any(s.has_error for s in self._sources.values())

    def get_errors(self) -> dict[int, str]:
        """Get all error messages."""
        return {
            index: source.error_message
            for index, source in self._sources.items()
            if source.has_error
        }
