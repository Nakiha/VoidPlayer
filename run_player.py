"""
VoidPlayer 播放器启动脚本

用法:
    python run_player.py                    # 默认模式 (包含调试功能)
    python run_player.py --profile perf     # 性能模式 (禁用调试功能)
    python run_player.py --profile debug    # 调试模式 (完整调试功能)
"""

# === 必须在 import PySide6 之前设置 ===
# 使用 ANGLE 后端 (OpenGL ES -> Direct3D)，避免 NVIDIA 游戏覆盖弹窗
import os
os.environ['QT_OPENGL'] = 'angle'

import sys
import ctypes
import argparse
import signal
import tracemalloc
from pathlib import Path
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QIcon, QSurfaceFormat
from qfluentwidgets import setThemeColor, setTheme, Theme

from player.core.logging_config import setup_logging, get_logger
from player.ui.main_window import MainWindow
from player.core.config import config, Profile


def parse_args():
    """解析命令行参数"""
    parser = argparse.ArgumentParser(
        description="VoidPlayer - 视频对比播放器",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    def profile_type(value):
        """大小写不敏感的 profile 参数"""
        val = value.lower()
        if val not in ("perf", "debug"):
            raise argparse.ArgumentTypeError(f"无效的 profile: {value} (可选: perf, debug)")
        return val

    parser.add_argument(
        "--profile", "-p",
        type=profile_type,
        default=None,
        help=(
            "运行模式: "
            "'perf' = 性能模式 (禁用调试功能), "
            "'debug' = 调试模式 (完整调试功能)"
        )
    )
    parser.add_argument(
        "-i", "--input",
        action="append",
        dest="input_files",
        default=[],
        help="初始加载的媒体文件 (可多次指定)"
    )
    return parser.parse_args()


def _set_windows_app_id():
    """设置 Windows 应用程序 ID，用于任务栏和任务管理器显示"""
    if sys.platform == "win32":
        app_id = "VoidPlayer.VideoPlayer.1"
        try:
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(app_id)
        except (AttributeError, OSError):
            pass


def get_windows_accent_color() -> str:
    """
    获取 Windows 系统主题色 (强调色)
    返回格式: #RRGGBB
    """
    try:
        import winreg

        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\DWM")
        value, _ = winreg.QueryValueEx(key, "AccentColor")

        # ABGR 格式: 0xAABBGGRR -> RGB
        r = value & 0xFF
        g = (value >> 8) & 0xFF
        b = (value >> 16) & 0xFF
        return f"#{r:02x}{g:02x}{b:02x}"

    except Exception:
        return "#0078d4"

def main():
    # 解析命令行参数
    args = parse_args()

    # 设置 Windows 应用程序 ID (必须在创建 QApplication 之前)
    _set_windows_app_id()

    # 初始化日志系统 (开发模式使用项目目录下的 logs 文件夹)
    # 设置 dev_mode=False 使用用户数据目录
    setup_logging(app_name="voidplayer", dev_mode=True)

    # 设置全局配置
    profile_map = {
        None: Profile.DEFAULT,
        "perf": Profile.PERF,
        "debug": Profile.DEBUG,
    }
    config.profile = profile_map.get(args.profile, Profile.DEFAULT)

    logger = get_logger()

    # debug 模式下启动内存跟踪
    if config.profile == Profile.DEBUG:
        tracemalloc.start()
        logger.info("debug 模式: 已启动 tracemalloc 内存跟踪")

    # 打印配置参数
    logger.info(f"启动配置: profile={config.profile.value}")

    # 打印 native 模块版本信息
    try:
        from player.native import voidview_native
        version = getattr(voidview_native, "__version__", "unknown")
        build_time = getattr(voidview_native, "__build_time__", "unknown")
        logger.info(f"native 模块: version={version}, build_time={build_time}")
    except ImportError as e:
        logger.warning(f"native 模块未加载: {e}")
    except Exception as e:
        logger.warning(f"获取 native 模块版本失败: {e}")

    try:
        _run_app(args.input_files, sys.argv[1:])
    except KeyboardInterrupt:
        logger.info("用户中断 (Ctrl+C)，程序退出")
        sys.exit(0)


def _run_app(input_files: list[str], launch_args: list[str]):
    """运行 Qt 应用"""

    # === 硬件加速和高刷适配配置 (必须在创建 QApplication 之前) ===

    # 启用高 DPI 缩放 (Qt6 默认启用，但显式设置更保险)
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough
    )

    # 设置 OpenGL 表面格式
    fmt = QSurfaceFormat()
    # 垂直同步: 0=关闭, 1=开启
    # 视频播放器建议开启，避免画面撕裂；高刷显示器会自动适配刷新率
    fmt.setSwapInterval(1)
    QSurfaceFormat.setDefaultFormat(fmt)

    # 在 Windows 上优先使用 ANGLE (DirectX 后端)，比纯 OpenGL 更稳定
    # 如果 ANGLE 不可用则回退到 OpenGL
    QApplication.setAttribute(Qt.AA_UseOpenGLES)

    app = QApplication(sys.argv)

    # 设置 Ctrl+C 信号处理
    def handle_sigint(*args):
        logger = get_logger()
        logger.info("用户中断 (Ctrl+C)，程序退出")
        app.quit()

    signal.signal(signal.SIGINT, handle_sigint)
    # 定时器让 Python 有机会处理信号
    timer = QTimer()
    timer.start(100)  # 每 100ms 检查一次
    timer.timeout.connect(lambda: None)

    # 设置应用图标
    icon_path = Path(__file__).parent / "resources" / "icons" / "icon.svg"
    if icon_path.exists():
        app.setWindowIcon(QIcon(str(icon_path)))

    # 获取并设置 Windows 系统主题色
    accent_color = get_windows_accent_color()
    setThemeColor(accent_color)

    # 设置主题跟随系统 (自动检测亮色/暗色)
    setTheme(Theme.AUTO)

    # 创建主窗口
    window = MainWindow(initial_files=input_files, launch_args=launch_args)
    window.show()

    sys.exit(app.exec())


if __name__ == "__main__":
    main()
