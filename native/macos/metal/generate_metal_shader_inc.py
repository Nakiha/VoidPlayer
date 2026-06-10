#!/usr/bin/env python3
"""Generate the runtime Metal shader include from editable .metal sources."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


SHADER_FILES = [
    "common_types.metal",
    "common_layout.metal",
    "common_color.metal",
    "common_overlay_sampling.metal",
    "layout_package.metal",
    "layout_cvpixelbuffer.metal",
    "common_overlay.metal",
    "overlay_direct_line.metal",
    "overlay_layer.metal",
    "overlay_legacy_composite.metal",
]


def generated_source(shader_dir: Path) -> str:
    body = "".join((shader_dir / name).read_text() for name in SHADER_FILES)
    return f'R"(\n{body})";\n'


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--shader-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "shaders",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent
        / "generated"
        / "metal_pixel_buffer_uploader_shaders.inc",
    )
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    expected = generated_source(args.shader_dir)
    if args.check:
        actual = args.output.read_text() if args.output.exists() else ""
        if actual != expected:
            print(
                "generated Metal shader include is stale; run "
                "native/macos/metal/generate_metal_shader_inc.py",
                file=sys.stderr,
            )
            return 1
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists() and args.output.read_text() == expected:
        return 0
    args.output.write_text(expected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
