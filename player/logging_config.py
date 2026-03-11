"""日志配置 - 支持日志轮转"""

import sys
from pathlib import Path
from loguru import logger


def setup_logging(
    app_name: str = "voidplayer",
    log_dir: Path | None = None,
    level: str = "INFO",
    rotation: str = "10 MB",
    retention: str = "7 days",
    compression: str = "zip",
    console_output: bool = True,
    dev_mode: bool = False,
) -> Path:
    """配置日志系统

    Args:
        app_name: 应用名称，用于日志文件名
        log_dir: 日志目录，None 时自动选择
        level: 日志级别
        rotation: 轮转大小/时间，如 "10 MB", "1 day", "00:00"
        retention: 保留时间，如 "7 days", "1 week"
        compression: 压缩格式，如 "zip", "gz"，空字符串表示不压缩
        console_output: 是否输出到控制台
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

    # 控制台输出格式
    console_format = (
        "<green>{time:YYYY-MM-DD HH:mm:ss}</green> | "
        "<level>{level: <8}</level> | "
        "<cyan>{name}</cyan>:<cyan>{function}</cyan>:<cyan>{line}</cyan> - "
        "<level>{message}</level>"
    )

    # 文件输出格式 (不带颜色)
    file_format = (
        "{time:YYYY-MM-DD HH:mm:ss} | "
        "{level: <8} | "
        "{name}:{function}:{line} - "
        "{message}"
    )

    # 添加控制台处理器
    if console_output:
        logger.add(
            sys.stderr,
            format=console_format,
            level=level,
            colorize=True,
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
    )

    logger.info(f"日志系统已初始化，日志目录: {log_dir}")

    return log_dir


def get_logger():
    """获取日志器"""
    return logger
