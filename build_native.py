#!/usr/bin/env python
"""构建 native 模块的脚本"""

import subprocess
import sys
import sysconfig
from pathlib import Path


def get_pybind11_dir() -> str:
    """获取 pybind11 的 cmake 目录"""
    import pybind11
    return pybind11.get_cmake_dir()


def get_output_dir() -> str:
    """获取 native 模块输出目录"""
    return str(Path(__file__).parent / "player" / "native")


def main():
    project_root = Path(__file__).parent
    build_dir = project_root / "native" / "build"

    # 确保 build 目录存在
    build_dir.mkdir(parents=True, exist_ok=True)

    pybind11_dir = get_pybind11_dir()
    output_dir = get_output_dir()

    # CMake 配置
    cmake_cmd = [
        "cmake",
        f"-Dpybind11_DIR={pybind11_dir}",
        f"-DVOIDVIEW_OUTPUT_DIR={output_dir}",
        str(project_root / "native"),
    ]

    # 构建
    build_cmd = ["cmake", "--build", ".", "--config", "Release"]

    print(f"=== Configuring (output: {output_dir}) ===")
    subprocess.run(cmake_cmd, cwd=build_dir, check=True)

    print("\n=== Building ===")
    subprocess.run(build_cmd, cwd=build_dir, check=True)

    print("\n=== Build completed ===")


if __name__ == "__main__":
    main()
