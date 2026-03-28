"""Build script for video_renderer_native module."""
import subprocess
import sys
import os
from pathlib import Path

def main():
    import pybind11
    pybind11_dir = pybind11.get_cmake_dir()

    script_dir = Path(__file__).parent
    build_dir = script_dir / "build-msvc"

    cmake_cmd = "cmake"

    cmake_args = [
        cmake_cmd,
        "-B", str(build_dir),
        "-S", str(script_dir),
        f"-Dpybind11_DIR={pybind11_dir}",
    ]

    print("Configuring...")
    subprocess.check_call(cmake_args)

    build_type = "Release"
    if "--debug" in sys.argv:
        build_type = "Debug"

    print(f"Building ({build_type})...")
    subprocess.check_call([
        cmake_cmd,
        "--build", str(build_dir),
        "--config", build_type,
        "--parallel",
    ])

    print("Done.")

if __name__ == "__main__":
    main()
