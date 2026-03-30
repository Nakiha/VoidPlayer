"""Build script for video_renderer_native module."""
import argparse
import subprocess
import sys
import os
from pathlib import Path


def configure(build_dir: Path, script_dir: Path):
    import pybind11
    pybind11_dir = pybind11.get_cmake_dir()

    cmake_args = [
        "cmake",
        "-B", str(build_dir),
        "-S", str(script_dir),
        f"-Dpybind11_DIR={pybind11_dir}",
    ]

    # Auto-detect FFmpeg from sibling project if not at default location
    default_ffmpeg = script_dir.parent / "libs" / "ffmpeg"
    if not (default_ffmpeg / "include" / "libavcodec" / "avcodec.h").exists():
        for candidate in [
            Path("D:/Code/yorune/VoidPlayer/libs/ffmpeg"),
            Path("D:/Code/yorune/VoidView/libs/ffmpeg"),
        ]:
            if (candidate / "include" / "libavcodec" / "avcodec.h").exists():
                cmake_args.append(f"-DFFMPEG_ROOT={candidate}")
                break

    subprocess.check_call(cmake_args)


def build(build_dir: Path, build_type: str):
    subprocess.check_call([
        "cmake",
        "--build", str(build_dir),
        "--config", build_type,
        "--parallel",
    ])


def test(build_dir: Path, build_type: str):
    subprocess.check_call([
        "ctest",
        "--test-dir", str(build_dir),
        "--build-config", build_type,
        "--output-on-failure",
    ])


def benchmark(build_dir: Path, build_type: str):
    exe = build_dir / build_type / "pipeline_bench.exe"
    video = build_dir.parent.parent / "resources" / "video" / "h264_9s_1920x1080.mp4"
    subprocess.check_call([str(exe), str(video)])


def main():
    parser = argparse.ArgumentParser(description="Build video_renderer_native")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true",
                      help="Only compile, skip tests")
    mode.add_argument("--test-only", action="store_true",
                      help="Skip compilation, only run tests")
    mode.add_argument("--benchmarks-only", action="store_true",
                      help="Only compile and run pipeline benchmarks")
    parser.add_argument("--debug", action="store_true",
                        help="Build in Debug mode (no optimization, with PDB debug symbols)")
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    build_dir = script_dir / "build-msvc"
    build_type = "Debug" if args.debug else "Release"

    if args.benchmarks_only:
        print("Configuring...")
        configure(build_dir, script_dir)

        print(f"Building ({build_type})...")
        build(build_dir, build_type)

        print("Running benchmarks...")
        benchmark(build_dir, build_type)
        print("Done.")
        return

    if not args.test_only:
        print("Configuring...")
        configure(build_dir, script_dir)

        print(f"Building ({build_type})...")
        build(build_dir, build_type)

    if not args.build_only:
        print("Running tests...")
        test(build_dir, build_type)

    print("Done.")


if __name__ == "__main__":
    main()
