#!/usr/bin/env python3
"""
VoidView Native 模块构建脚本

用法:
    cd native
    python build.py [--release] [--debug]
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description='Build voidview_native module')
    parser.add_argument('--release', action='store_true', default=True,
                        help='Build in Release mode')
    parser.add_argument('--debug', action='store_true',
                        help='Build in Debug mode')
    return parser.parse_args()


def get_python_info():
    """获取 Python 路径信息"""
    python_exe = sys.executable

    # 获取 site-packages 路径
    result = subprocess.run(
        [python_exe, '-c', 'import site_packages; print(site_packages.getsitepackage("pybind11").location)'],
        capture_output=True,
        text=True
    )
    pybind11_dir = result.stdout.strip()

    return python_exe, pybind11_dir


def build(args):
    """执行 CMake 构建"""
    project_root = Path(__file__).parent
    build_dir = project_root / 'build'

    # 清理旧构建
    if build_dir.exists():
        print(f"Cleaning {build_dir}...")
        shutil.rmtree(build_dir)

    build_dir.mkdir()
    (build_dir / 'Release').mkdir(exist_ok=True)

    # 获取 Python 信息
    python_exe, pybind11_dir = get_python_info()

    # 构建类型
    build_type = 'Release' if args.release else 'Debug'

    # CMake 配置
    cmake_args = [
        'cmake',
        '-B', str(build_dir),
        '-S', str(project_root),
        f'-DCMAKE_BUILD_TYPE={build_type}',
        f'-DPython_EXECUTABLE={python_exe}',
        f'-Dpybind11_DIR={pybind11_dir}',
        f'-DFFMPEG_ROOT={project_root / "../libs/ffmpeg"}',
    ]

    print("Running CMake configuration...")
    result = subprocess.run(cmake_args, check=True)

    # 构建
    print(f"Building in {build_type} mode...")
    build_args = [
        'cmake',
        '--build', str(build_dir),
        '--config', build_type,
        '--parallel',
        str(os.cpu_count() or '4'),  # 并行编译
    ]

    result = subprocess.run(build_args, check=True)

    # 复制输出文件
    output_dir = build_dir / build_type

    if sys.platform == 'win32':
        pyd_file = output_dir / 'voidview_native.pyd'
        if pyd_file.exists():
            dest = project_root.parent / 'player' / 'voidview_native.pyd'
            os.makedirs(dest.parent, exist_ok=True)
            shutil.copy2(pyd_file, dest)
            print(f"Copied {pyd_file} -> {dest}")
        else:
            print(f"Error: Output file not found: {pyd_file}")
            sys.exit(1)
    else:
        # Linux/macOS
        so_file = output_dir / 'libvoidview_native.so'
        if so_file.exists():
            dest = project_root.parent / 'player' / 'voidview_native.so'
            shutil.copy2(so_file, dest)
            print(f"Copied {so_file} -> {dest}")


def main():
    args = parse_args()
    build(args)

    print("\nBuild complete!")
    print("Output: player/voidview_native.pyd")
    print("\nRun tests with:")
    print("  python tests/test_opengl_demo.py")


