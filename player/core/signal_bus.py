"""
SignalBus - 全局信号总线
集中管理跨组件通信信号，解耦各模块
"""
from PySide6.QtCore import QObject, Signal


class SignalBus(QObject):
    """
    全局信号总线

    集中管理所有跨组件信号，避免组件间直接耦合。
    所有信号都通过此单例传递。
    """

    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    # ========== 播放控制信号 ==========

    # 播放状态
    play_requested = Signal()
    pause_requested = Signal()
    seek_requested = Signal(int)  # timestamp_ms

    # 速度/缩放
    speed_changed = Signal(int)   # speed_combo index
    zoom_changed = Signal(int)    # zoom_combo index

    # 循环
    loop_toggled = Signal(bool)

    # ========== 文件操作信号 ==========

    media_add_requested = Signal(str)   # file_path
    media_remove_requested = Signal(int)  # index
    media_files_selected = Signal(list)   # file paths

    # ========== 视图信号 ==========

    view_mode_changed = Signal(object)  # ViewMode
    fullscreen_toggled = Signal()

    # ========== 同步偏移信号 ==========

    sync_offset_changed = Signal(int, int)  # index, offset_ms

    # ========== 调试信号 ==========

    debug_monitor_requested = Signal()

    # ========== 窗口信号 ==========

    new_window_requested = Signal()


# 全局单例
signal_bus = SignalBus()
