#!/usr/bin/env python
"""
VoidPlayer 统一构建脚本

用法:
    python build.py native          # 构建 native 模块
    python build.py nuitka          # Nuitka 打包
    python build.py package         # 打包分发 (便携版 + 安装包)
    python build.py all             # 完整构建流程

    # package 子命令选项
    python build.py package --portable    # 仅便携版
    python build.py package --installer   # 仅安装包
"""

import argparse
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

# ============================================================
# 通用工具
# ============================================================

PROJECT_ROOT = Path(__file__).parent


def get_version() -> str:
    """从 pyproject.toml 获取版本号"""
    pyproject = PROJECT_ROOT / "pyproject.toml"
    content = pyproject.read_text(encoding="utf-8")
    match = re.search(r'version\s*=\s*"([^"]+)"', content)
    if match:
        return match.group(1)
    return "0.0.0"


# ============================================================
# Native 模块构建
# ============================================================

def get_pybind11_dir() -> str:
    """获取 pybind11 的 cmake 目录"""
    import pybind11
    return pybind11.get_cmake_dir()


def get_native_output_dir() -> str:
    """获取 native 模块输出目录"""
    return str(PROJECT_ROOT / "player" / "native")


def detect_vs_generator() -> str:
    """检测可用的 Visual Studio 生成器"""
    result = subprocess.run(
        ["cmake", "--help"],
        capture_output=True,
        text=True,
    )
    for line in result.stdout.splitlines():
        if "Visual Studio" in line and "[arch]" not in line:
            parts = line.strip().split("=")
            if len(parts) >= 2:
                return parts[0].strip().lstrip("* ")
    return ""


def remove_path_with_retry(path: Path) -> bool:
    """删除文件/目录，带重试机制"""
    if not path.exists():
        return True
    for attempt in range(5):
        try:
            shutil.rmtree(path) if path.is_dir() else path.unlink()
            return True
        except PermissionError:
            if attempt == 4:
                print(f"Warning: Could not remove {path}, please close any programs using it")
                return False
            import time
            time.sleep(0.5 * (attempt + 1))
    return False


def get_cached_generator(cache_file: Path) -> str | None:
    """从 CMakeCache.txt 获取生成器"""
    if not cache_file.exists():
        return None
    for line in cache_file.read_text(encoding="utf-8").splitlines():
        if line.startswith("CMAKE_GENERATOR:INTERNAL="):
            return line.split("=", 1)[1]
    return None


def check_generator_mismatch(build_dir: Path) -> bool:
    """检查缓存生成器是否与当前环境匹配，不匹配则清理缓存"""
    cache_file = build_dir / "CMakeCache.txt"
    cached_generator = get_cached_generator(cache_file)
    if not cached_generator:
        return False

    current_generator = detect_vs_generator()
    if cached_generator == current_generator:
        # 主项目生成器匹配，但需要检查 _deps 子模块
        return check_deps_generator_mismatch(build_dir, current_generator)

    print(f"=== Generator changed: {cached_generator} -> {current_generator}, cleaning cache ===")
    for name in ["CMakeCache.txt", "CMakeFiles", "_deps"]:
        remove_path_with_retry(build_dir / name)
    return True


def check_deps_generator_mismatch(build_dir: Path, expected_generator: str) -> bool:
    """检查 _deps 子模块的生成器是否匹配"""
    deps_dir = build_dir / "_deps"
    if not deps_dir.exists():
        return False

    # 检查 _deps 下所有 *-build 子目录的 CMakeCache.txt
    for subdirs in deps_dir.glob("*-build"):
        cache_file = subdirs / "CMakeCache.txt"
        cached_gen = get_cached_generator(cache_file)
        if cached_gen and cached_gen != expected_generator:
            print(f"=== _deps generator mismatch ({subdirs.name}: {cached_gen} != {expected_generator}), cleaning _deps ===")
            return remove_path_with_retry(deps_dir)

    return False


def build_native() -> int:
    """构建 native 模块"""
    build_dir = PROJECT_ROOT / "native" / "build"
    build_dir.mkdir(parents=True, exist_ok=True)

    check_generator_mismatch(build_dir)

    pybind11_dir = get_pybind11_dir()
    output_dir = get_native_output_dir()

    cmake_cmd = [
        "cmake",
        f"-Dpybind11_DIR={pybind11_dir}",
        f"-DVOIDVIEW_OUTPUT_DIR={output_dir}",
        str(PROJECT_ROOT / "native"),
    ]

    build_cmd = ["cmake", "--build", ".", "--config", "Release"]

    try:
        print(f"=== Configuring (output: {output_dir}) ===")
        subprocess.run(cmake_cmd, cwd=build_dir, check=True)

        print("\n=== Building ===")
        subprocess.run(build_cmd, cwd=build_dir, check=True)

        print("\n=== Build completed ===")
        return 0
    except subprocess.CalledProcessError as e:
        print(f"\n=== Build failed: {e} ===")
        return 1


# ============================================================
# Nuitka 打包
# ============================================================

def check_native_module():
    """检查 native 模块是否已构建"""
    native_dir = PROJECT_ROOT / "player" / "native"
    native_modules = list(native_dir.glob("voidview_native*.pyd"))
    if native_modules:
        print(f"发现 native 模块: {native_modules[0]}")
        return True
    else:
        print("提示: native 模块未构建，视频解码功能将不可用")
        print("      如需完整功能，请先运行: python build.py native")
        return False


def build_nuitka(onefile: bool = False) -> int:
    """使用 Nuitka 构建应用程序"""
    output_dir = PROJECT_ROOT / "build" / "dist"

    check_native_module()

    cmd = [
        sys.executable, "-m", "nuitka",
        "--mode=onefile" if onefile else "--mode=standalone",
        "--plugin-enable=pyside6",
        f"--output-dir={output_dir}",
        "--output-filename=VoidPlayer",
        "--windows-console-mode=attach",
        "--include-package=player",
        "--include-package-data=qfluentwidgets_nuitka",
        "--include-package-data=qfluentwidgets_nuitka._rc",
        "--include-package-data=qfluentwidgets_nuitka.common",
        "--include-package-data=qfluentwidgets_nuitka.components",
        "--include-package-data=qfluentwidgets_nuitka.window",
        "--include-package-data=qfluentwidgets_nuitka.multimedia",
        "--include-package=shiboken6",
        "--include-package=loguru",
        "--include-package=psutil",
        "--include-package=OpenGL",
        "--nofollow-import-to=OpenGL.GLES",
        "--nofollow-import-to=OpenGL.GLES2",
        "--nofollow-import-to=OpenGL.GLES3",
        "--nofollow-import-to=OpenGL.GLX",
        "--nofollow-import-to=OpenGL.WGL",
        "--nofollow-import-to=OpenGL.GLUT",
        "--nofollow-import-to=OpenGL_accelerate",
        "--lto=yes",
        "--deployment",
        "--enable-plugin=anti-bloat",
        "--show-progress",
        "--show-memory",
        "--assume-yes-for-downloads",
        str(PROJECT_ROOT / "run_player.py"),
    ]

    # 图标
    icon_path = PROJECT_ROOT / "resources" / "icons" / "icon.ico"
    if icon_path.exists():
        cmd.insert(cmd.index("--windows-console-mode=attach") + 1,
                   f"--windows-icon-from-ico={icon_path}")

    # FFmpeg DLLs
    ffmpeg_bin = PROJECT_ROOT / "libs" / "ffmpeg" / "bin"
    if ffmpeg_bin.exists():
        for dll in ffmpeg_bin.glob("*.dll"):
            cmd.append(f"--include-data-file={dll}=libs/ffmpeg/bin/{dll.name}")

    # 资源文件
    icons_dir = PROJECT_ROOT / "resources" / "icons"
    if icons_dir.exists():
        cmd.append(f"--include-data-dir={icons_dir}=resources/icons")

    # Native 模块
    native_dir = PROJECT_ROOT / "player" / "native"
    if native_dir.exists():
        for f in native_dir.iterdir():
            if f.suffix in ('.py', '.pyd'):
                cmd.append(f"--include-data-file={f}=player/native/{f.name}")

    # Shaders
    shaders_dir = PROJECT_ROOT / "player" / "shaders"
    if shaders_dir.exists():
        cmd.append(f"--include-data-dir={shaders_dir}=player/shaders")

    print("=" * 60)
    print("Nuitka 构建命令:")
    print("=" * 60)
    print(" \\\n    ".join(cmd))
    print("=" * 60)
    print(f"模式: {'onefile' if onefile else 'standalone'}")
    print(f"输出目录: {output_dir}")
    print("=" * 60)

    result = subprocess.run(cmd, cwd=PROJECT_ROOT)
    return result.returncode


# ============================================================
# 打包分发
# ============================================================

def build_portable() -> Path:
    """构建便携版 (zip 压缩包)"""
    dist_dir = PROJECT_ROOT / "build" / "dist"
    nuitka_output = dist_dir / "run_player.dist"
    output_dir = PROJECT_ROOT / "build" / "release"

    if not nuitka_output.exists():
        print("错误: Nuitka 构建产物不存在，请先运行: python build.py nuitka")
        sys.exit(1)

    version = get_version()
    archive_name = f"VoidPlayer-{version}-Portable"
    archive_path = output_dir / f"{archive_name}.zip"

    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"创建便携版: {archive_path}")

    with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        for file_path in nuitka_output.rglob("*"):
            if file_path.is_file():
                arcname = f"{archive_name}/{file_path.relative_to(nuitka_output)}"
                zf.write(file_path, arcname)

    print(f"便携版创建完成: {archive_path}")
    return archive_path


def build_installer() -> Path:
    """构建安装包 (Inno Setup)"""
    dist_dir = PROJECT_ROOT / "build" / "dist"
    nuitka_output = dist_dir / "run_player.dist"
    output_dir = PROJECT_ROOT / "build" / "release"
    iss_template = PROJECT_ROOT / "installer.iss"

    if not nuitka_output.exists():
        print("错误: Nuitka 构建产物不存在，请先运行: python build.py nuitka")
        sys.exit(1)

    if not iss_template.exists():
        print(f"错误: Inno Setup 模板不存在: {iss_template}")
        sys.exit(1)

    version = get_version()
    output_dir.mkdir(parents=True, exist_ok=True)

    # 生成临时 ISS 文件
    iss_content = iss_template.read_text(encoding="utf-8")
    iss_content = iss_content.replace("{{VERSION}}", version)
    iss_content = iss_content.replace("{{PROJECT_ROOT}}", str(PROJECT_ROOT))
    iss_content = iss_content.replace("{{OUTPUT_DIR}}", str(output_dir))

    temp_iss = output_dir / "temp_installer.iss"
    temp_iss.write_text(iss_content, encoding="utf-8")

    # 查找 ISCC
    iscc_paths = [
        Path("C:/Program Files (x86)/Inno Setup 6/ISCC.exe"),
        Path("C:/Program Files/Inno Setup 6/ISCC.exe"),
        shutil.which("ISCC"),
    ]

    iscc = None
    for path in iscc_paths:
        if path and Path(path).exists():
            iscc = Path(path)
            break

    if not iscc:
        print("错误: 未找到 ISCC.exe，请安装 Inno Setup 6")
        print("下载地址: https://jrsoftware.org/isdl.php")
        sys.exit(1)

    print(f"使用 Inno Setup: {iscc}")
    print("正在编译安装包...")

    result = subprocess.run([str(iscc), str(temp_iss)], cwd=output_dir)

    temp_iss.unlink(missing_ok=True)

    if result.returncode != 0:
        print("安装包编译失败")
        sys.exit(result.returncode)

    installer_path = output_dir / f"VoidPlayer-{version}-Setup.exe"
    print(f"安装包创建完成: {installer_path}")
    return installer_path


# ============================================================
# 命令入口
# ============================================================

def cmd_native(args):
    return build_native()


def cmd_nuitka(args):
    return build_nuitka(onefile=args.onefile)


def cmd_package(args):
    results = []

    if not args.installer_only:
        results.append(("便携版", build_portable()))
    if not args.portable_only:
        results.append(("安装包", build_installer()))

    print("\n" + "=" * 50)
    print("打包完成:")
    for name, path in results:
        print(f"  {name}: {path}")
    print("=" * 50)
    return 0


def cmd_all(args):
    print("=" * 50)
    print("步骤 1/3: 构建 native 模块")
    print("=" * 50)
    ret = build_native()
    if ret != 0:
        return ret

    print("\n" + "=" * 50)
    print("步骤 2/3: Nuitka 打包")
    print("=" * 50)
    ret = build_nuitka(onefile=False)
    if ret != 0:
        return ret

    print("\n" + "=" * 50)
    print("步骤 3/3: 打包分发")
    print("=" * 50)
    return cmd_package(args)


def main():
    parser = argparse.ArgumentParser(
        description="VoidPlayer 统一构建脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # native
    p_native = subparsers.add_parser("native", help="构建 native 模块")

    # nuitka
    p_nuitka = subparsers.add_parser("nuitka", help="Nuitka 打包")
    p_nuitka.add_argument("--onefile", action="store_true", help="构建单文件版本")

    # package
    p_package = subparsers.add_parser("package", help="打包分发 (便携版 + 安装包)")
    p_package.add_argument("--portable", dest="portable_only", action="store_true",
                           help="仅生成便携版")
    p_package.add_argument("--installer", dest="installer_only", action="store_true",
                           help="仅生成安装包")

    # all
    p_all = subparsers.add_parser("all", help="完整构建流程 (native + nuitka + package)")
    p_all.add_argument("--portable", dest="portable_only", action="store_true",
                       help="仅生成便携版")
    p_all.add_argument("--installer", dest="installer_only", action="store_true",
                       help="仅生成安装包")

    args = parser.parse_args()

    commands = {
        "native": cmd_native,
        "nuitka": cmd_nuitka,
        "package": cmd_package,
        "all": cmd_all,
    }

    return commands[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
