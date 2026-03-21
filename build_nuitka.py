#!/usr/bin/env python
"""
VoidPlayer Nuitka 打包脚本

使用方法:
    python build_nuitka.py              # 构建 standalone 版本
    python build_nuitka.py --onefile    # 构建 onefile 版本 (需要 zstd)

依赖:
    pip install nuitka
    pip install zstd  # 仅 onefile 模式需要
"""

import argparse
import subprocess
import sys
from pathlib import Path


def get_site_packages_path() -> Path:
    """获取 site-packages 路径"""
    result = subprocess.run(
        [sys.executable, "-c", "import site; print(site.getsitepackages()[0])"],
        capture_output=True,
        text=True,
    )
    return Path(result.stdout.strip())


def check_native_module():
    """检查 native 模块是否已构建"""
    native_module = Path(__file__).parent / "player" / "native" / "voidview_native.pyd"
    # 也检查带 Python 版本的 pyd 文件
    native_modules = list((Path(__file__).parent / "player" / "native").glob("voidview_native*.pyd"))
    if native_modules:
        print(f"发现 native 模块: {native_modules[0]}")
        return True
    else:
        print("提示: native 模块未构建，视频解码功能将不可用")
        print("      如需完整功能，请先运行: python build_native.py")
        return False


def build_standalone(onefile: bool = False):
    """使用 Nuitka 构建应用程序"""

    project_root = Path(__file__).parent

    # 输出目录
    output_dir = project_root / "build" / "dist"

    # 检查 native 模块
    check_native_module()

    # 基础命令
    cmd = [
        sys.executable, "-m", "nuitka",
        "--mode=onefile" if onefile else "--mode=standalone",
        # PySide6 插件
        "--plugin-enable=pyside6",
        # 输出配置
        f"--output-dir={output_dir}",
        "--output-filename=VoidPlayer",
        # Windows 配置: attach 模式 - 有控制台时附加（方便调试），无控制台时隐藏（正常双击启动）
        "--windows-console-mode=attach",
        # 包含整个 player 包
        "--include-package=player",
        # PySide6-Fluent-Widgets-Nuitka 配置 (Nuitka 兼容版本)
        "--include-package-data=qfluentwidgets_nuitka",
        "--include-package-data=qfluentwidgets_nuitka._rc",
        "--include-package-data=qfluentwidgets_nuitka.common",
        "--include-package-data=qfluentwidgets_nuitka.components",
        "--include-package-data=qfluentwidgets_nuitka.window",
        "--include-package-data=qfluentwidgets_nuitka.multimedia",
        # PySide6-Essential 和 shiboken6
        "--include-package=shiboken6",
        # 其他依赖
        "--include-package=loguru",
        "--include-package=psutil",
        # OpenGL - 包含必要的模块
        # 注意: OpenGL_accelerate 是 Cython 编译的加速模块，与 Nuitka 不兼容
        # 排除后 PyOpenGL 会自动使用纯 Python 实现，性能影响很小
        "--include-package=OpenGL",
        "--nofollow-import-to=OpenGL.GLES",
        "--nofollow-import-to=OpenGL.GLES2",
        "--nofollow-import-to=OpenGL.GLES3",
        "--nofollow-import-to=OpenGL.GLX",
        "--nofollow-import-to=OpenGL.WGL",
        "--nofollow-import-to=OpenGL.GLUT",
        "--nofollow-import-to=OpenGL_accelerate",
        # 性能优化
        "--lto=yes",
        # 部署模式
        "--deployment",
        # 启用插件
        "--enable-plugin=anti-bloat",
        # 显示进度
        "--show-progress",
        "--show-memory",
        # 主脚本
        str(project_root / "run_player.py"),
    ]

    # 检查图标文件 (需要 .ico 格式)
    icon_path = project_root / "resources" / "icons" / "icon.ico"
    icon_svg = project_root / "resources" / "icons" / "icon.svg"
    if icon_path.exists():
        cmd.insert(cmd.index("--windows-console-mode=attach") + 1,
                   f"--windows-icon-from-ico={icon_path}")
    elif icon_svg.exists():
        # SVG 不能直接用，需要转换或跳过
        print("提示: 图标文件需要 .ico 格式，当前只有 .svg，跳过图标设置")

    # 添加 FFmpeg DLLs (如果存在)
    ffmpeg_bin = project_root / "libs" / "ffmpeg" / "bin"
    if ffmpeg_bin.exists():
        for dll in ffmpeg_bin.glob("*.dll"):
            cmd.append(f"--include-data-file={dll}=libs/ffmpeg/bin/{dll.name}")

    # 添加资源文件 (排除测试视频)
    resources_dir = project_root / "resources"
    if resources_dir.exists():
        # 添加图标资源
        icons_dir = resources_dir / "icons"
        if icons_dir.exists():
            cmd.append(f"--include-data-dir={icons_dir}=resources/icons")
        # 注意: 排除 resources/video 目录，这些是测试视频，不需要打包

    # 添加 native 模块包 (包含 __init__.py 和 pyd)
    native_dir = project_root / "player" / "native"
    if native_dir.exists():
        for f in native_dir.iterdir():
            if f.suffix in ('.py', '.pyd'):
                cmd.append(f"--include-data-file={f}=player/native/{f.name}")

    # 添加 shaders 目录
    shaders_dir = project_root / "player" / "shaders"
    if shaders_dir.exists():
        cmd.append(f"--include-data-dir={shaders_dir}=player/shaders")

    # 输出命令
    print("=" * 60)
    print("Nuitka 构建命令:")
    print("=" * 60)
    print(" \\\n    ".join(cmd))
    print("=" * 60)
    print(f"模式: {'onefile' if onefile else 'standalone'}")
    print(f"输出目录: {output_dir}")
    print("=" * 60)

    # 执行构建
    result = subprocess.run(cmd, cwd=project_root)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(
        description="VoidPlayer Nuitka 打包脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
    python build_nuitka.py              # 构建 standalone 版本 (推荐用于调试)
    python build_nuitka.py --onefile    # 构建 onefile 版本 (单文件分发)
        """,
    )
    parser.add_argument(
        "--onefile",
        action="store_true",
        help="构建单文件版本 (需要安装 zstd)",
    )
    args = parser.parse_args()

    returncode = build_standalone(onefile=args.onefile)
    sys.exit(returncode)


if __name__ == "__main__":
    main()
