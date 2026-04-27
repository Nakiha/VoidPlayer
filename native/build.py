"""Build script for video_renderer_native module."""
import argparse
import os
import subprocess
import sys
from pathlib import Path


def is_ffmpeg_root(path: Path) -> bool:
    return (path / "include" / "libavcodec" / "avcodec.h").exists()


def resolve_ffmpeg_root(script_dir: Path, explicit_root: str | None) -> Path:
    candidates: list[Path] = []

    if explicit_root:
        candidates.append(Path(explicit_root))

    for env_name in ("FFMPEG_ROOT", "FFMPEG_DIR"):
        env_value = os.environ.get(env_name)
        if env_value:
            candidates.append(Path(env_value))

    candidates.append(script_dir.parent / "windows" / "libs" / "ffmpeg")

    for candidate in candidates:
        resolved = candidate.expanduser().resolve()
        if is_ffmpeg_root(resolved):
            return resolved

    checked = "\n  - ".join(str(path.expanduser().resolve()) for path in candidates)
    raise FileNotFoundError(
        "FFmpeg headers were not found. Checked:\n"
        f"  - {checked}\n"
        "Provide a valid FFmpeg root with --ffmpeg-root or FFMPEG_ROOT."
    )


def configure(
    build_dir: Path,
    script_dir: Path,
    ffmpeg_root: Path,
    build_benchmarks: bool = False,
    build_tests: bool = True,
    build_python: bool = True,
    build_ffi: bool = True,
):
    import pybind11
    pybind11_dir = pybind11.get_cmake_dir()

    cmake_args = [
        "cmake",
        "-B", str(build_dir),
        "-S", str(script_dir),
        f"-Dpybind11_DIR={pybind11_dir}",
        f"-DFFMPEG_ROOT={ffmpeg_root}",
        f"-DBUILD_BENCHMARKS={'ON' if build_benchmarks else 'OFF'}",
        f"-DBUILD_TESTS={'ON' if build_tests else 'OFF'}",
        f"-DBUILD_PYTHON={'ON' if build_python else 'OFF'}",
        f"-DBUILD_FFI={'ON' if build_ffi else 'OFF'}",
    ]

    subprocess.check_call(cmake_args)


def build(build_dir: Path, build_type: str):
    subprocess.check_call([
        "cmake",
        "--build", str(build_dir),
        "--config", build_type,
        "--parallel",
    ])


def test(build_dir: Path, build_type: str, script_dir: Path):
    subprocess.check_call([
        "ctest",
        "--test-dir", str(build_dir),
        "--build-config", build_type,
        "-V",
        "--timeout", "180",
        "--output-on-failure",
    ])

    repo_root = script_dir.parent
    analysis_test = script_dir / "analysis" / "tests" / "python" / "test_analysis_formats.py"
    analysis_generate = build_dir / build_type / "analysis_generate.exe"
    env = os.environ.copy()
    env["VOID_ANALYSIS_GENERATE_EXE"] = str(analysis_generate)
    subprocess.check_call([
        sys.executable,
        "-m", "pytest",
        str(analysis_test),
        "-q",
    ], cwd=repo_root, env=env)


def benchmark(build_dir: Path, build_type: str):
    exe = build_dir / build_type / "pipeline_bench.exe"
    video = build_dir.parents[1] / "resources" / "video" / "h264_9s_1920x1080.mp4"
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
    parser.add_argument("--ffmpeg-root", type=str, default=None,
                        help="Path to an FFmpeg root containing include/ and lib/")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    build_dir = (script_dir / "build-msvc").resolve()
    build_type = "Debug" if args.debug else "Release"
    ffmpeg_root = resolve_ffmpeg_root(script_dir, args.ffmpeg_root)

    if args.benchmarks_only:
        print("Configuring...", flush=True)
        configure(
            build_dir,
            script_dir,
            ffmpeg_root,
            build_benchmarks=True,
            build_tests=False,
            build_python=False,
            build_ffi=False,
        )

        print(f"Building ({build_type})...", flush=True)
        build(build_dir, build_type)

        print("Running benchmarks...", flush=True)
        benchmark(build_dir, build_type)
        print("Done.", flush=True)
        return

    if not args.test_only:
        print("Configuring...", flush=True)
        configure(build_dir, script_dir, ffmpeg_root)

        print(f"Building ({build_type})...", flush=True)
        build(build_dir, build_type)

    if not args.build_only:
        print("Running tests...", flush=True)
        test(build_dir, build_type, script_dir)

    print("Done.", flush=True)


if __name__ == "__main__":
    main()
