"""Internal helper for standalone native CMake builds."""
import argparse
import os
import sys
import subprocess
from pathlib import Path


def host_platform() -> str:
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "portable"


def default_build_dir(script_dir: Path, platform_name: str) -> Path:
    build_root = script_dir.parent / "build" / "native" / "standalone"
    if platform_name == "windows":
        return build_root / "windows-msvc"
    if platform_name == "macos":
        return build_root / "macos"
    return build_root / "portable"


def default_ffmpeg_candidates(script_dir: Path, platform_name: str) -> list[Path]:
    toolchain_root = script_dir.parent / ".toolchains" / "ffmpeg"
    if platform_name == "windows":
        return [
            toolchain_root / "windows-x64",
        ]
    if platform_name == "macos":
        return [
            toolchain_root / "macos-arm64",
        ]
    return [
        toolchain_root / "macos-arm64",
        toolchain_root / "windows-x64",
    ]


def is_ffmpeg_root(path: Path) -> bool:
    return (path / "include" / "libavcodec" / "avcodec.h").exists()


def resolve_ffmpeg_root(
    script_dir: Path,
    platform_name: str,
    explicit_root: str | None,
) -> Path:
    candidates: list[Path] = []

    if explicit_root:
        candidates.append(Path(explicit_root))

    for env_name in ("FFMPEG_ROOT", "FFMPEG_DIR"):
        env_value = os.environ.get(env_name)
        if env_value:
            candidates.append(Path(env_value))

    candidates.extend(default_ffmpeg_candidates(script_dir, platform_name))

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


def env_flag(name: str) -> bool:
    value = os.environ.get(name, "")
    return value.lower() in {"1", "true", "yes", "on"}


def configure(
    build_dir: Path,
    script_dir: Path,
    ffmpeg_root: Path,
    build_tests: bool = True,
    build_analysis_tests: bool = True,
    build_python: bool = True,
    build_ffi: bool = True,
    use_local_deps: bool = False,
):
    cmake_args = [
        "cmake",
        "-B", str(build_dir),
        "-S", str(script_dir),
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DFFMPEG_ROOT={ffmpeg_root}",
        f"-DBUILD_TESTS={'ON' if build_tests else 'OFF'}",
        f"-DBUILD_ANALYSIS_TESTS={'ON' if build_analysis_tests else 'OFF'}",
        f"-DBUILD_PYTHON={'ON' if build_python else 'OFF'}",
        f"-DBUILD_FFI={'ON' if build_ffi else 'OFF'}",
    ]
    if use_local_deps:
        cmake_args.append("-DVOID_USE_LOCAL_DEPS=ON")

    if build_python:
        try:
            import pybind11
        except ImportError:
            pass
        else:
            cmake_args.append(f"-Dpybind11_DIR={pybind11.get_cmake_dir()}")

    subprocess.check_call(cmake_args)


def build(build_dir: Path, build_type: str):
    subprocess.check_call([
        "cmake",
        "--build", str(build_dir),
        "--config", build_type,
        "--parallel",
    ])


def test(build_dir: Path, build_type: str, script_dir: Path, skip_analysis_tests: bool = False):
    ctest_cmd = [
        "ctest",
        "--test-dir", str(build_dir),
        "--build-config", build_type,
        "-V",
        "--timeout", "180",
        "--output-on-failure",
    ]
    if skip_analysis_tests:
        ctest_cmd.extend(["-E", "^analysis_tests$"])
    subprocess.check_call(ctest_cmd)

    if skip_analysis_tests:
        return


def native_executable_name(name: str, platform_name: str) -> str:
    return f"{name}.exe" if platform_name == "windows" else name


def main():
    parser = argparse.ArgumentParser(description="Build VoidPlayer standalone native targets")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true",
                      help="Only compile, skip tests")
    mode.add_argument("--test-only", action="store_true",
                      help="Skip compilation, only run tests")
    parser.add_argument("--debug", action="store_true",
                        help="Build in Debug mode (no optimization, with PDB debug symbols)")
    parser.add_argument("--ffmpeg-root", type=str, default=None,
                        help="Path to an FFmpeg root containing include/ and lib/")
    parser.add_argument("--platform", choices=["host", "windows", "macos", "portable"],
                        default="host",
                        help="Native target platform defaults to the current host")
    parser.add_argument("--build-dir", type=str, default=None,
                        help="Override the native build directory")
    parser.add_argument("--github", action="store_true",
                        help="Skip external analysis tool tests for lightweight GitHub CI")
    parser.add_argument("--use-local-deps", action="store_true",
                        help="Use native/_deps dependency sources instead of FetchContent downloads")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    platform_name = host_platform() if args.platform == "host" else args.platform
    build_dir = (
        Path(args.build_dir)
        if args.build_dir
        else default_build_dir(script_dir, platform_name)
    ).expanduser().resolve()
    build_type = "Debug" if args.debug else "Release"
    ffmpeg_root = resolve_ffmpeg_root(script_dir, platform_name, args.ffmpeg_root)
    use_local_deps = args.use_local_deps or env_flag("VOID_USE_LOCAL_DEPS")

    if not args.test_only:
        print("Configuring...", flush=True)
        configure(
            build_dir,
            script_dir,
            ffmpeg_root,
            build_analysis_tests=not args.github,
            use_local_deps=use_local_deps,
        )

        print(f"Building ({build_type})...", flush=True)
        build(build_dir, build_type)

    if not args.build_only:
        print("Running tests...", flush=True)
        test(build_dir, build_type, script_dir, skip_analysis_tests=args.github)

    print("Done.", flush=True)


if __name__ == "__main__":
    main()
