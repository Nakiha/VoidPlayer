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


def detect_vs_generator() -> str:
    """检测可用的 Visual Studio 生成器"""
    result = subprocess.run(
        ["cmake", "--help"],
        capture_output=True,
        text=True,
    )
    for line in result.stdout.splitlines():
        if "Visual Studio" in line and "[arch]" not in line:
            # 提取生成器名称，如 "Visual Studio 17 2022"
            parts = line.strip().split("=")
            if len(parts) >= 2:
                # 移除可能的前缀标记（如 "*"）
                return parts[0].strip().lstrip("* ")
    return ""


def check_generator_mismatch(build_dir: Path) -> bool:
    """检查缓存生成器是否与当前环境匹配，不匹配则清理缓存"""
    import shutil

    cache_file = build_dir / "CMakeCache.txt"
    if not cache_file.exists():
        return False

    # 从缓存读取生成器
    cached_generator = None
    for line in cache_file.read_text(encoding="utf-8").splitlines():
        if line.startswith("CMAKE_GENERATOR:INTERNAL="):
            cached_generator = line.split("=", 1)[1]
            break

    if not cached_generator:
        return False

    # 检测当前生成器
    current_generator = detect_vs_generator()

    if cached_generator != current_generator:
        print(f"=== Generator changed: {cached_generator} -> {current_generator}, cleaning cache ===")
        # Windows 上可能有文件锁，重试几次
        for name in ["CMakeCache.txt", "CMakeFiles", "_deps"]:
            path = build_dir / name
            if not path.exists():
                continue
            for attempt in range(3):
                try:
                    shutil.rmtree(path) if path.is_dir() else path.unlink()
                    break
                except PermissionError:
                    if attempt == 2:
                        print(f"Warning: Could not remove {path}, please close any programs using it")
                    else:
                        import time
                        time.sleep(0.5)
        return True

    return False


def main():
    project_root = Path(__file__).parent
    build_dir = project_root / "native" / "build"

    # 确保 build 目录存在
    build_dir.mkdir(parents=True, exist_ok=True)

    # 检查生成器变化，自动清理缓存
    check_generator_mismatch(build_dir)

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
