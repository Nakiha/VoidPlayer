"""
SignalBus - 全局信号总线
集中管理跨组件通信信号，解耦各模块

信号分类：
1. 请求信号 (xxx_requested): 用户操作 → 请求处理者响应
2. 更新信号 (xxx_updated/xxx_changed): 状态变化 → 通知订阅者
3. 同步信号: 用于组件间状态同步
"""
from PySide6.QtCore import QObject, Signal


class SignalBus(QObject):
    """
    全局信号总线

    集中管理所有跨组件信号，避免组件间直接耦合。
    所有信号都通过此单例传递。

    使用原则:
    - UI 组件只发信号，不直接调用业务逻辑
    - 业务逻辑组件监听信号并处理
    - 状态更新通过信号广播，UI 组件监听更新
    """

    _instance = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    # ========== 播放控制请求 ==========

    play_requested = Signal()
    pause_requested = Signal()
    seek_requested = Signal(int)  # ms, 快速 seek (keyframe)
    precise_seek_requested = Signal(int)  # ms, 精确 seek (frame-accurate)
    prev_frame_requested = Signal()
    next_frame_requested = Signal()
    loop_toggled = Signal(bool)

    # ========== 速度/缩放 ==========

    speed_changed = Signal(float)  # 播放速度 (0.25, 0.5, 1.0, 1.5, 2.0)
    zoom_changed = Signal(int)  # 缩放百分比

    # ========== 媒体操作请求 ==========

    media_add_requested = Signal(str)  # file_path
    media_add_dialog_requested = Signal()  # 请求打开文件选择对话框
    media_remove_requested = Signal(int)  # index
    media_files_selected = Signal(list)  # file paths (从对话框选择后)

    # ========== 轨道操作请求 ==========

    track_swap_requested = Signal(int, int)  # index1, index2
    track_move_requested = Signal(int, int)  # old_index, new_index
    track_visibility_changed = Signal(int, bool)  # index, visible
    track_mute_changed = Signal(int, bool)  # index, muted
    sync_offset_changed = Signal(int, int)  # index, offset_ms

    # ========== 视图请求 ==========

    view_mode_changed = Signal(object)  # ViewMode
    fullscreen_toggled = Signal()

    # ========== 窗口请求 ==========

    settings_requested = Signal()
    debug_monitor_requested = Signal()
    new_window_requested = Signal()

    # ========== 播放状态更新 (从 MainWindow/DecoderPool 广播) ==========

    duration_updated = Signal(int)  # ms
    position_updated = Signal(int)  # ms
    eof_reached = Signal()
    decoder_error = Signal(int, str)  # track_index, message
    frame_ready = Signal()  # 所有轨道解码完成一帧

    # ========== UI 同步 ==========

    playhead_position_changed = Signal(float)  # 0.0 ~ 1.0
    playing_state_changed = Signal(bool)  # is_playing


# 全局单例
signal_bus = SignalBus()
