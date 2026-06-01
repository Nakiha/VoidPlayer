"""macOS FFmpeg bundle layout helpers."""

from __future__ import annotations

from pathlib import Path


FFMPEG_RUNTIME_LIBRARIES = (
    "libavcodec",
    "libavformat",
    "libavutil",
    "libswresample",
)


def _version_key(path: Path) -> tuple[int, ...]:
    stem = path.name.removesuffix(".dylib")
    parts = stem.split(".")[1:]
    values: list[int] = []
    for part in parts:
        try:
            values.append(int(part))
        except ValueError:
            values.append(-1)
    return tuple(values)


def _real_dylib_for(lib_dir: Path, library: str) -> str:
    matches = [
        path
        for path in lib_dir.glob(f"{library}.*.dylib")
        if path.is_file() and not path.is_symlink()
    ]
    if not matches:
        raise RuntimeError(f"missing macOS FFmpeg dylib for {library} in {lib_dir}")
    return max(matches, key=_version_key).name


def ffmpeg_runtime_dylibs(lib_dir: Path) -> list[str]:
    """Return versioned FFmpeg dylibs expected in the app bundle."""
    return [_real_dylib_for(lib_dir, library) for library in FFMPEG_RUNTIME_LIBRARIES]


def _major_symlink_name(versioned_name: str) -> str | None:
    stem = versioned_name.removesuffix(".dylib")
    parts = stem.split(".")
    if len(parts) < 2 or not parts[1].isdigit():
        return None
    return f"{parts[0]}.{parts[1]}.dylib"


def ffmpeg_runtime_symlinks(lib_dir: Path) -> list[str]:
    """Return dylib symlinks derived from the discovered FFmpeg versions."""
    names: list[str] = []
    for library, dylib in zip(FFMPEG_RUNTIME_LIBRARIES, ffmpeg_runtime_dylibs(lib_dir)):
        major = _major_symlink_name(dylib)
        if major is not None:
            names.append(major)
        names.append(f"{library}.dylib")

    for name in names:
        path = lib_dir / name
        if not path.is_symlink():
            raise RuntimeError(f"missing macOS FFmpeg dylib symlink: {path}")
    return names
