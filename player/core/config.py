"""
AppConfig - 全局配置管理

使用单例模式，全局可访问:
    from player.config import config, Profile

    if config.profile == Profile.DEBUG:
        ...
    if config.profile != Profile.PERF:
        ...
"""
from enum import Enum


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


# 全局单例
config = AppConfig()
