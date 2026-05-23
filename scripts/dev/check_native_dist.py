"""Smoke-check native distribution folders."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def _require(path: Path, label: str) -> None:
    if not path.is_file():
        raise RuntimeError(f"missing {label}: {path}")
    if path.stat().st_size <= 0:
        raise RuntimeError(f"{label} is empty: {path}")


def _has_ffmpeg_license(path: Path) -> bool:
    return (
        (path / "LICENSE").is_file() or
        (path / "LICENSE.txt").is_file() or
        (path / "LICENSES" / "FFmpeg-LICENSE.md").is_file()
    )


def check_ffi_dist(path: Path) -> None:
    _require(path / "video_renderer_ffi.dll", "FFI DLL")
    _require(path / "ffi_exports.h", "FFI header")
    _require(path / "README.txt", "FFmpeg README")
    if not _has_ffmpeg_license(path):
        raise RuntimeError(
            f"missing FFmpeg LICENSE, LICENSE.txt, or LICENSES/FFmpeg-LICENSE.md in {path}")
    for dll in ("avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "swresample-6.dll"):
        _require(path / dll, dll)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ffi", type=Path, required=True,
                        help="Path to native dist/ffi directory")
    args = parser.parse_args()
    try:
        check_ffi_dist(args.ffi)
    except RuntimeError as exc:
        print(f"ERROR: {exc}")
        return 1
    print("native dist smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
