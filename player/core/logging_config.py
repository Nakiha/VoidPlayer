"""日志配置 - 支持日志轮转和全局异常捕获"""

import faulthandler
import logging
import sys
import threading
import traceback
import warnings
from pathlib import Path

from loguru import logger

# faulthandler 文件句柄 (保持打开直到程序结束)
_crash_file = None


def _is_console_available() -> bool:
    """检测控制台是否可用（可以安全写入 stderr）

    日志输出到 stderr，所以只需要检测 stderr 是否可用。
    非打包模式下始终返回 True（开发调试需要日志输出）。
    """
    if not sys.stderr:
        return False

    # 非打包模式：始终启用控制台输出
    is_frozen = hasattr(sys, 'frozen') or "__compiled__" in globals()
    if not is_frozen:
        return True

    # Windows: 检查 stderr 是否连接到真正的控制台
    if sys.platform == "win32":
        try:
            import ctypes
            kernel32 = ctypes.windll.kernel32
            stderr_handle = kernel32.GetStdHandle(0xFFFFFFF4)  # STD_ERROR_HANDLE

            if stderr_handle == -1:  # INVALID_HANDLE_VALUE
                return False

            # GetConsoleMode 只有在真正的控制台上才会成功
            mode = ctypes.c_ulong()
            return kernel32.GetConsoleMode(stderr_handle, ctypes.byref(mode)) != 0
        except Exception:
            return False

    # Unix: 检查 stderr 是否是 tty
    return sys.stderr.isatty()


def _setup_qt_handler():
    """设置 Qt 消息处理器"""
    try:
        from PySide6.QtCore import qInstallMessageHandler, QtMsgType

        _levels = {
            QtMsgType.QtDebugMsg: "debug",
            QtMsgType.QtWarningMsg: "warning",
            QtMsgType.QtCriticalMsg: "error",
            QtMsgType.QtFatalMsg: "critical",
            QtMsgType.QtInfoMsg: "info",
        }

        def qt_handler(msg_type, context, msg):
            level = _levels.get(msg_type, "debug")
            file_info = f"{context.file}:{context.line}" if context.file else ""
            logger.log(level, f"[Qt] {msg} ({file_info})")

        qInstallMessageHandler(qt_handler)
        return True
    except ImportError:
        return False


class _LoguruHandler(logging.Handler):
    """将标准 logging 转发到 loguru"""

    def emit(self, record: logging.LogRecord):
        try:
            level = logger.level(record.levelname).name
        except ValueError:
            level = record.levelno

        # 从 record 中提取位置信息，放到消息前缀
        filename = Path(record.pathname).name if record.pathname else record.name
        line = record.lineno or 0

        # 使用特殊前缀标记，让 filter 函数提取
        logger.opt(depth=0, exception=record.exc_info).log(
            level, f"\x00{filename}:{line}\x00{record.getMessage()}"
        )


def _warning_handler(message, category, filename, lineno, _file=None, _line=None):
    """将 warnings 模块的警告重定向到 loguru"""
    logger.warning(f"{category.__name__}: {message} ({filename}:{lineno})")


def _exception_hook(exc_type, exc_value, exc_tb):
    """全局异常钩子 - 捕获未处理的异常并记录到日志"""
    tb_str = ''.join(traceback.format_exception(exc_type, exc_value, exc_tb))
    logger.critical(f"未捕获的异常:\n{tb_str}")
    # 调用默认的异常处理器
    sys.__excepthook__(exc_type, exc_value, exc_tb)


def _thread_exception_hook(args):
    """线程异常钩子 - 捕获线程中未处理的异常"""
    tb_str = ''.join(traceback.format_exception(args.exc_type, args.exc_value, args.exc_tb))
    thread_name = args.thread.name if args.thread else "unknown"
    logger.critical(f"线程 [{thread_name}] 未捕获的异常:\n{tb_str}")


def setup_logging(
    app_name: str = "voidplayer",
    log_dir: Path | None = None,
    level: str = "INFO",
    ffmpeg_level: str = "INFO",
    rotation: str = "10 MB",
    retention: str = "7 days",
    compression: str = "zip",
    dev_mode: bool = False,
) -> Path:
    """配置日志系统

    Args:
        app_name: 应用名称，用于日志文件名
        log_dir: 日志目录，None 时自动选择
        level: 默认日志级别
        ffmpeg_level: FFmpeg 日志级别
        rotation: 轮转大小/时间，如 "10 MB", "1 day", "00:00"
        retention: 保留时间，如 "7 days", "1 week"
        compression: 压缩格式，如 "zip", "gz"，空字符串表示不压缩
        dev_mode: 开发模式，日志落盘到项目目录的 logs 文件夹

    Returns:
        日志目录路径
    """
    # 移除默认的处理器
    logger.remove()

    # 确定日志目录
    if log_dir is None:
        if dev_mode:
            # 开发模式: 使用项目目录下的 logs 文件夹
            # 向上查找包含 player 目录的根目录
            current_dir = Path(__file__).resolve().parent
            while current_dir != current_dir.parent:
                if (current_dir / "player").exists():
                    break
                current_dir = current_dir.parent
            log_dir = current_dir / "logs"
        else:
            # 生产模式: 使用用户数据目录
            if sys.platform == "win32":
                base_dir = Path.home() / "AppData" / "Local" / "VoidPlayer"
            elif sys.platform == "darwin":
                base_dir = Path.home() / "Library" / "Logs" / "VoidPlayer"
            else:
                base_dir = Path.home() / ".local" / "share" / "VoidPlayer"
            log_dir = base_dir / "logs"

    log_dir.mkdir(parents=True, exist_ok=True)

    # 格式化过滤器: 添加 short_file (只显示文件名)
    import re
    _prefix_pattern = re.compile(r"^\x00([^:\x00]+):(\d+)\x00")

    def format_filter(record):
        # 检查是否有特殊前缀 (来自 _LoguruHandler)
        msg = record["message"]
        match = _prefix_pattern.match(msg)
        if match:
            record["extra"]["short_file"] = match.group(1)
            record["extra"]["line"] = int(match.group(2))
            # 移除前缀
            record["message"] = msg[match.end():]
        else:
            # 普通 loguru 调用
            if record["file"]:
                record["extra"]["short_file"] = record["file"].name
            else:
                record["extra"]["short_file"] = record["name"]
            record["extra"]["line"] = record["line"]
        return True

    # 控制台输出格式
    console_format = (
        "<green>{time:YYYY-MM-DD HH:mm:ss.SSS}</green> | "
        "<level>{level: <8}</level> | "
        "<cyan>{extra[short_file]}</cyan>:<cyan>{extra[line]}</cyan> - "
        "<level>{message}</level>"
    )

    # 文件输出格式 (不带颜色)
    file_format = (
        "{time:YYYY-MM-DD HH:mm:ss.SSS} | "
        "{level: <8} | "
        "{extra[short_file]}:{extra[line]} - "
        "{message}"
    )

    # 检测控制台是否可用
    console_available = _is_console_available()

    # 仅在控制台可用时添加控制台处理器
    if console_available:
        logger.add(
            sys.stderr,
            format=console_format,
            level=level,
            colorize=True,
            filter=format_filter,
        )

    # 添加文件处理器
    log_file = log_dir / f"{app_name}.log"
    logger.add(
        str(log_file),
        format=file_format,
        level=level,
        rotation=rotation,
        retention=retention,
        compression=compression if compression else None,
        encoding="utf-8",
        filter=format_filter,
    )

    # 打印 loguru 配置信息
    logger.info(
        f"loguru 日志配置: path={log_file}, level={level}, "
        f"rotation={rotation}, retention={retention}, compression={compression or 'none'}"
    )

    # 安装全局异常钩子
    sys.excepthook = _exception_hook
    threading.excepthook = _thread_exception_hook

    # 将 warnings 重定向到日志
    warnings.showwarning = _warning_handler

    # 将标准 logging 重定向到 loguru (第三方库可能使用)
    logging.basicConfig(handlers=[_LoguruHandler()], level=logging.DEBUG, force=True)

    # 启用 faulthandler (捕获 C 级崩溃)
    global _crash_file
    crash_path = log_dir / f"{app_name}_crash.log"
    _crash_file = crash_path.open("w", encoding="utf-8")
    faulthandler.enable(file=_crash_file)

    # 设置 Qt 消息处理器 (延迟调用，需在 QApplication 创建后)
    _setup_qt_handler()

    # 配置 native 模块日志
    _setup_native_logging(log_dir, app_name, level, ffmpeg_level, console_available)

    return log_dir


def _setup_native_logging(
    log_dir: Path,
    app_name: str,
    level: str,
    ffmpeg_level: str,
    console_available: bool
):
    """配置 native 模块日志"""
    try:
        from player.native import voidview_native

        # 映射 loguru 级别到 native 级别
        level_map = {
            "TRACE": 0,
            "DEBUG": 1,
            "INFO": 2,
            "SUCCESS": 2,
            "WARNING": 3,
            "ERROR": 4,
            "CRITICAL": 5,
        }
        native_level = level_map.get(level.upper(), 2)
        ffmpeg_native_level = level_map.get(ffmpeg_level.upper(), 2)

        # 初始化 native 日志系统
        voidview_native.init_logging(native_level, ffmpeg_native_level)

        # 先添加控制台 sink，这样后续 add_file_sink 的日志也能输出到控制台
        if console_available:
            voidview_native.add_console_sink()

        # 设置 native 日志文件
        native_log_path = log_dir / f"{app_name}_native.log"
        voidview_native.add_file_sink(str(native_log_path))

    except ImportError:
        pass  # native 模块未安装，忽略
    except Exception as e:
        logger.warning(f"配置 native 日志失败: {e}")


def get_logger():
    """获取日志器"""
    return logger
