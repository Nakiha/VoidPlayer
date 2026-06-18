"""Check release/package license notice files.

This script intentionally checks file presence and a few key strings only. It is
not a legal review; it prevents obvious release artifacts from shipping without
the FFmpeg GPL package notices and VoidPlayer third-party manifests.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _require_file(path: Path, label: str) -> None:
    if not path.is_file():
        raise RuntimeError(f"missing {label}: {path}")


def _require_text(path: Path, needles: list[str], label: str) -> None:
    _require_file(path, label)
    text = path.read_text(encoding="utf-8", errors="ignore")
    missing = [needle for needle in needles if needle not in text]
    if missing:
        joined = ", ".join(repr(value) for value in missing)
        raise RuntimeError(f"{label} is missing expected text: {joined}")


def _has_ffmpeg_license(root: Path) -> bool:
    return (
        (root / "LICENSE").is_file() or
        (root / "LICENSE.txt").is_file() or
        (root / "LICENSES" / "FFmpeg-LICENSE.md").is_file()
    )


def _require_windows_ffmpeg_readme(path: Path) -> None:
    _require_file(path, "Windows FFmpeg package README")
    text = path.read_text(encoding="utf-8", errors="ignore")
    if "FFmpeg" not in text:
        raise RuntimeError("Windows FFmpeg package README is missing expected text: 'FFmpeg'")
    if "configuration" not in text and "Target: windows-x64-msvc" not in text:
        raise RuntimeError(
            "Windows FFmpeg package README is missing expected text: "
            "'configuration' or 'Target: windows-x64-msvc'")


def check_source_tree() -> None:
    _require_text(ROOT / "LICENSE", ["GNU GENERAL PUBLIC LICENSE", "Version 3"],
                  "top-level GPL license")
    _require_text(ROOT / "THIRD_PARTY_NOTICES.md",
                  ["FFmpeg Runtime Package", "native/THIRD_PARTY_NATIVE.md"],
                  "top-level third-party notices")
    _require_text(ROOT / "native" / "THIRD_PARTY_NATIVE.md",
                  ["FFmpeg runtime/dev package", "GPL v3 package"],
                  "native third-party manifest")

    windows_ffmpeg_root = ROOT / ".toolchains" / "ffmpeg" / "windows-x64"
    _require_windows_ffmpeg_readme(windows_ffmpeg_root / "README.txt")
    if not _has_ffmpeg_license(windows_ffmpeg_root):
        raise RuntimeError(
            "missing FFmpeg LICENSE, LICENSE.txt, or LICENSES/FFmpeg-LICENSE.md "
            f"in {windows_ffmpeg_root}")

    macos_ffmpeg_root = ROOT / ".toolchains" / "ffmpeg" / "macos-arm64"
    _require_text(macos_ffmpeg_root / "README.txt",
                  ["FFmpeg", "Target: macos-arm64", "VideoToolbox"],
                  "macOS FFmpeg package README")
    _require_file(macos_ffmpeg_root / "voidplayer-ffmpeg-manifest.json",
                  "macOS FFmpeg package manifest")
    if not (macos_ffmpeg_root / "LICENSES" / "FFmpeg-LICENSE.md").is_file():
        raise RuntimeError(
            f"missing FFmpeg-LICENSE.md in {macos_ffmpeg_root / 'LICENSES'}")


def check_stage(stage_dir: Path) -> None:
    _require_file(stage_dir / "README.txt", "staged FFmpeg README")
    if not _has_ffmpeg_license(stage_dir):
        raise RuntimeError(
            f"missing staged FFmpeg LICENSE, LICENSE.txt, or LICENSES/FFmpeg-LICENSE.md in {stage_dir}")

    docs = stage_dir / "docs"
    _require_text(docs / "LICENSE", ["GNU GENERAL PUBLIC LICENSE", "Version 3"],
                  "staged GPL license")
    _require_text(docs / "THIRD_PARTY_NOTICES.md",
                  ["FFmpeg Runtime Package", "native/THIRD_PARTY_NATIVE.md"],
                  "staged third-party notices")
    _require_text(docs / "THIRD_PARTY_NATIVE.md",
                  ["FFmpeg runtime/dev package", "GPL v3 package"],
                  "staged native third-party manifest")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=Path, default=None,
                        help="Optional staged package directory to validate")
    args = parser.parse_args()

    try:
        check_source_tree()
        if args.stage is not None:
            check_stage(args.stage)
    except RuntimeError as exc:
        print(f"ERROR: {exc}")
        return 1

    print("release compliance smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
