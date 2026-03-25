"""
AppConfig - 全局配置管理

使用单例模式，全局可访问:
    from player.core.config import config, Profile

    if config.profile == Profile.DEBUG:
        ...
    if config.profile != Profile.PERF:
        ...
    if config.is_opengl_debug_enabled:
        ...
"""
from enum import Enum
from typing import Literal

# 日志级别类型
LogLevel = Literal["TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]


class Profile(Enum):
    """运行模式"""
    DEFAULT = "default"  # 默认模式
    PERF = "perf"        # 性能模式 (禁用调试)
    DEBUG = "debug"      # 调试模式 (自动内存追踪)


class AppConfig:
    """全局配置 (单例)"""

    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
            cls._instance._init()
        return cls._instance

    def _init(self):
        self.profile = Profile.DEFAULT
        self._log_levels: dict[str, LogLevel] = {
            "default": "INFO",
            "ffmpeg": "INFO",
            "opengl": "INFO",
        }

    def set_log_levels(self, levels: dict[str, str]):
        """设置日志级别 (从命令行参数)"""
        for key, val in levels.items():
            if key in self._log_levels:
                self._log_levels[key] = val

    def get_log_level(self, category: str) -> LogLevel:
        """获取指定分类的日志级别"""
        return self._log_levels.get(category, "INFO")

    @property
    def is_opengl_debug_enabled(self) -> bool:
        """OpenGL debug output 是否启用 (opengl=DEBUG/TRACE)"""
        return self._log_levels.get("opengl", "INFO") in ("DEBUG", "TRACE")


# 全局单例
config = AppConfig()
