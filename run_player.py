"""
VoidPlayer 播放器启动脚本
"""

# === 必须在 import PySide6 之前设置 ===
# 使用 ANGLE 后端 (OpenGL ES -> Direct3D)
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
from qfluentwidgets_nuitka import setThemeColor, setTheme, Theme

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

    def log_level_type(value):
        """解析日志级别，支持键值对格式: DEBUG 或 default=DEBUG,ffmpeg=INFO"""
        valid_levels = ["TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]
        result = {}

        # 检查是否是键值对格式
        if "=" in value:
            pairs = value.split(",")
            for pair in pairs:
                if "=" in pair:
                    key, val = pair.split("=", 1)
                    key = key.strip().lower()
                    val = val.strip().upper()
                    if key not in ("default", "ffmpeg"):
                        raise argparse.ArgumentTypeError(f"无效的日志类型: {key} (可选: default, ffmpeg)")
                    if val not in valid_levels:
                        raise argparse.ArgumentTypeError(f"无效的日志级别: {val} (可选: {', '.join(valid_levels)})")
                    result[key] = val
                else:
                    raise argparse.ArgumentTypeError(f"无效的格式: {pair} (期望: key=value)")
        else:
            # 简单格式，仅设置 default
            val = value.upper()
            if val not in valid_levels:
                raise argparse.ArgumentTypeError(f"无效的日志级别: {val} (可选: {', '.join(valid_levels)})")
            result["default"] = val

        return result

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
    parser.add_argument(
        "--auto-play",
        action="store_true",
        help="启动完成后自动开始播放"
    )
    parser.add_argument(
        "--mock", "-m",
        type=str,
        default=None,
        help="运行自动化测试脚本 (.vpmock 文件)"
    )
    parser.add_argument(
        "--list-actions",
        action="store_true",
        help="列出所有可用动作"
    )
    parser.add_argument(
        "--log-level", "-l",
        type=log_level_type,
        default={},
        help=(
            "日志级别，支持格式: "
            "DEBUG (仅 default), "
            "default=DEBUG, "
            "default=DEBUG,ffmpeg=INFO (分别设置). "
            "默认: default=INFO, ffmpeg=INFO"
        )
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
    log_levels = args.log_level
    default_level = log_levels.get("default", "INFO")
    ffmpeg_level = log_levels.get("ffmpeg", "INFO")
    setup_logging(app_name="voidplayer", dev_mode=True, level=default_level, ffmpeg_level=ffmpeg_level)

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
        module_path = getattr(voidview_native, "__file__", "unknown")
        logger.info(f"native 模块: version={version}, build_time={build_time}, path={module_path}")
    except ImportError as e:
        logger.warning(f"native 模块未加载: {e}")
    except Exception as e:
        logger.warning(f"获取 native 模块版本失败: {e}")

    try:
        _run_app(args.input_files, args.auto_play, sys.argv[1:], args.mock, args.list_actions)
    except KeyboardInterrupt:
        logger.info("用户中断 (Ctrl+C)，程序退出")
        sys.exit(0)


def _run_app(input_files: list[str], auto_play: bool, launch_args: list[str], mock_script: str = None, list_actions: bool = False):
    """运行 Qt 应用"""

    # 列出所有动作并退出
    if list_actions:
        _list_available_actions()
        return

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
    window = MainWindow(
        initial_files=input_files,
        auto_play=auto_play,
        launch_args=launch_args,
        mock_script=mock_script,
    )
    window.show()

    sys.exit(app.exec())


def _list_available_actions():
    """列出所有可用动作 (从 actions 动态读取)"""
    from player.core.actions import get_actions_by_category, get_action_metadata

    print("VoidPlayer 可用动作列表:")
    print("=" * 60)

    actions = get_action_metadata()
    actions_by_category = get_actions_by_category(actions)

    for category, cat_actions in actions_by_category.items():
        print(f"\n{category}:")
        print("-" * 40)
        for action in cat_actions:
            print(f"  {action.name:<20} {action.format_params()}")
            print(f"    {action.description}")

    print("\n" + "=" * 60)
    print(".vpmock 脚本格式: 时间偏移(秒), 动作名称[, 参数1, 参数2, ...]")
    print("示例:")
    print("  1.0, PLAY")
    print("  3.0, SEEK_TO, 5000")
    print("  5.0, ADD_TRACK, test/video.mp4")


if __name__ == "__main__":
    main()
