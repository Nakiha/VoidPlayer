"""
ActionRegistry - 动作注册表
"""
from typing import TYPE_CHECKING

from .types import (
    ActionDef, ParamDef,
    CATEGORY_BASIC, CATEGORY_PLAYBACK, CATEGORY_SPEED_ZOOM,
    CATEGORY_TRACK, CATEGORY_VIEW, CATEGORY_DEBUG, CATEGORY_ASSERT,
)
from .resolvers import resolve_file_picker, resolve_multi_file_picker, resolve_save_path

if TYPE_CHECKING:
    from player.ui.main_window import MainWindow


def get_action_metadata() -> list[ActionDef]:
    """
    获取动作元数据列表 (不绑定 MainWindow)

    用于 --list-actions 显示帮助信息
    """
    noop = lambda *a, **k: None

    return [
        # 基础动作
        ActionDef("WAIT", noop, [ParamDef("duration_ms", int)],
                  "等待指定时间 (毫秒)", CATEGORY_BASIC),
        ActionDef("QUIT", noop, [ParamDef("exit_code", int, default=0)],
                  "退出程序", CATEGORY_BASIC),

        # 播放控制
        ActionDef("PLAY", noop, [], "开始播放", CATEGORY_PLAYBACK),
        ActionDef("PAUSE", noop, [], "暂停播放", CATEGORY_PLAYBACK),
        ActionDef("PLAY_PAUSE", noop, [], "切换播放/暂停", CATEGORY_PLAYBACK),
        ActionDef("STOP", noop, [], "停止播放并回到开头", CATEGORY_PLAYBACK),
        ActionDef("SEEK_TO", noop, [ParamDef("timestamp_ms", int)],
                  "Seek 到指定时间点 (毫秒)", CATEGORY_PLAYBACK),
        ActionDef("SEEK_FORWARD", noop, [ParamDef("delta_ms", int, default=5000)],
                  "前进 (毫秒)", CATEGORY_PLAYBACK),
        ActionDef("SEEK_BACKWARD", noop, [ParamDef("delta_ms", int, default=5000)],
                  "后退 (毫秒)", CATEGORY_PLAYBACK),
        ActionDef("SEEK_RELATIVE", noop, [ParamDef("delta_ms", int)],
                  "相对 Seek (毫秒，正数前进，负数后退)", CATEGORY_PLAYBACK),
        ActionDef("PREV_FRAME", noop, [], "上一帧", CATEGORY_PLAYBACK),
        ActionDef("NEXT_FRAME", noop, [], "下一帧", CATEGORY_PLAYBACK),
        ActionDef("TOGGLE_LOOP", noop, [], "切换循环", CATEGORY_PLAYBACK),

        # 速度/缩放
        ActionDef("SPEED_UP", noop, [], "加速", CATEGORY_SPEED_ZOOM),
        ActionDef("SPEED_DOWN", noop, [], "减速", CATEGORY_SPEED_ZOOM),
        ActionDef("SPEED_SET", noop, [ParamDef("index", int)],
                  "设置速度索引", CATEGORY_SPEED_ZOOM),
        ActionDef("ZOOM_IN", noop, [], "放大", CATEGORY_SPEED_ZOOM),
        ActionDef("ZOOM_OUT", noop, [], "缩小", CATEGORY_SPEED_ZOOM),
        ActionDef("ZOOM_SET", noop, [ParamDef("index", int)],
                  "设置缩放索引", CATEGORY_SPEED_ZOOM),

        # 轨道管理
        ActionDef("ADD_TRACK", noop, [ParamDef("file_path", str)],
                  "添加媒体轨道", CATEGORY_TRACK),
        ActionDef("ADD_TRACKS", noop, [ParamDef("file_paths", list)],
                  "添加多个轨道", CATEGORY_TRACK),
        ActionDef("REMOVE_TRACK", noop, [ParamDef("index", int)],
                  "移除指定轨道", CATEGORY_TRACK),
        ActionDef("SET_OFFSET", noop, [ParamDef("index", int), ParamDef("offset_ms", int)],
                  "设置轨道偏移 (毫秒)", CATEGORY_TRACK),
        ActionDef("SWAP_TRACKS", noop, [ParamDef("index1", int), ParamDef("index2", int)],
                  "交换轨道", CATEGORY_TRACK),
        ActionDef("CLEAR_TRACKS", noop, [], "清空所有轨道", CATEGORY_TRACK),

        # 视图控制
        ActionDef("SET_VIEW_MODE", noop, [ParamDef("mode", str)],
                  "设置视图模式 (SINGLE, SIDE_BY_SIDE, COMPARISON)", CATEGORY_VIEW),
        ActionDef("TOGGLE_FULLSCREEN", noop, [], "切换全屏", CATEGORY_VIEW),
        ActionDef("NEW_WINDOW", noop, [], "新建窗口", CATEGORY_VIEW),

        # 调试/诊断
        ActionDef("TOGGLE_DEBUG_MONITOR", noop, [], "切换调试监控窗口", CATEGORY_DEBUG),
        ActionDef("TOGGLE_STATS", noop, [], "切换性能统计窗口", CATEGORY_DEBUG),
        ActionDef("SCREENSHOT", noop, [ParamDef("save_path", str, default="")],
                  "保存当前帧截图", CATEGORY_DEBUG),

        # 断言动作
        ActionDef("ASSERT_PLAYING", noop, [], "断言正在播放", CATEGORY_ASSERT),
        ActionDef("ASSERT_PAUSED", noop, [], "断言已暂停", CATEGORY_ASSERT),
        ActionDef("ASSERT_POSITION", noop,
                  [ParamDef("expected_ms", int), ParamDef("tolerance_ms", int, default=100)],
                  "断言播放位置", CATEGORY_ASSERT),
        ActionDef("ASSERT_TRACK_COUNT", noop, [ParamDef("expected", int)],
                  "断言轨道数量", CATEGORY_ASSERT),
    ]


def create_action_registry(mw: "MainWindow") -> list[ActionDef]:
    """创建动作注册表 (绑定 MainWindow)"""
    actions = []

    # 基础动作
    actions.append(ActionDef("WAIT", lambda duration_ms: None,
        [ParamDef("duration_ms", int)], "等待指定时间 (毫秒)", CATEGORY_BASIC))
    actions.append(ActionDef("QUIT", lambda exit_code=0: _quit_app(exit_code),
        [ParamDef("exit_code", int, default=0)], "退出程序", CATEGORY_BASIC))

    # 播放控制
    actions.append(ActionDef("PLAY", mw.play, [], "开始播放", CATEGORY_PLAYBACK))
    actions.append(ActionDef("PAUSE", mw.pause, [], "暂停播放", CATEGORY_PLAYBACK))
    actions.append(ActionDef("PLAY_PAUSE", _toggle_play_pause(mw), [],
        "切换播放/暂停", CATEGORY_PLAYBACK))
    actions.append(ActionDef("STOP", lambda: (mw.pause(), mw.seek_to(0)), [],
        "停止播放并回到开头", CATEGORY_PLAYBACK))
    actions.append(ActionDef("SEEK_TO", mw.seek_to, [ParamDef("timestamp_ms", int)],
        "Seek 到指定时间点 (毫秒)", CATEGORY_PLAYBACK))
    actions.append(ActionDef("SEEK_FORWARD",
        lambda delta_ms=5000: mw.seek_to(mw._decoder_pool.position_ms + delta_ms),
        [ParamDef("delta_ms", int, default=5000)], "前进 (毫秒)", CATEGORY_PLAYBACK))
    actions.append(ActionDef("SEEK_BACKWARD",
        lambda delta_ms=5000: mw.seek_to(max(0, mw._decoder_pool.position_ms - delta_ms)),
        [ParamDef("delta_ms", int, default=5000)], "后退 (毫秒)", CATEGORY_PLAYBACK))
    actions.append(ActionDef("SEEK_RELATIVE",
        lambda delta_ms: mw.seek_to(max(0, mw._decoder_pool.position_ms + delta_ms)),
        [ParamDef("delta_ms", int)], "相对 Seek (毫秒，正数前进，负数后退)", CATEGORY_PLAYBACK))
    actions.append(ActionDef("PREV_FRAME", mw._decoder_pool.prev_frame, [],
        "上一帧", CATEGORY_PLAYBACK))
    actions.append(ActionDef("NEXT_FRAME", mw._decoder_pool.next_frame, [],
        "下一帧", CATEGORY_PLAYBACK))
    actions.append(ActionDef("TOGGLE_LOOP", lambda: mw.controls_bar.loop_btn.toggle(), [],
        "切换循环", CATEGORY_PLAYBACK))

    # 速度/缩放
    actions.append(ActionDef("SPEED_UP", _speed_up(mw), [], "加速", CATEGORY_SPEED_ZOOM))
    actions.append(ActionDef("SPEED_DOWN", _speed_down(mw), [], "减速", CATEGORY_SPEED_ZOOM))
    actions.append(ActionDef("SPEED_SET",
        lambda index: mw.controls_bar.speed_combo.setCurrentIndex(index),
        [ParamDef("index", int)], "设置速度索引", CATEGORY_SPEED_ZOOM))
    actions.append(ActionDef("ZOOM_IN", _zoom_in(mw), [], "放大", CATEGORY_SPEED_ZOOM))
    actions.append(ActionDef("ZOOM_OUT", _zoom_out(mw), [], "缩小", CATEGORY_SPEED_ZOOM))
    actions.append(ActionDef("ZOOM_SET",
        lambda index: mw.controls_bar.zoom_combo.setCurrentIndex(index),
        [ParamDef("index", int)], "设置缩放索引", CATEGORY_SPEED_ZOOM))

    # 轨道管理
    actions.append(ActionDef("ADD_TRACK", mw.add_media, [ParamDef("file_path", str)],
        resolve_file_picker, "添加媒体轨道", CATEGORY_TRACK))
    actions.append(ActionDef("ADD_TRACKS",
        lambda file_paths: [mw.add_media(p) for p in file_paths],
        [ParamDef("file_paths", list)], resolve_multi_file_picker, "添加多个轨道", CATEGORY_TRACK))
    actions.append(ActionDef("REMOVE_TRACK", mw.remove_media, [ParamDef("index", int)],
        None, "移除指定轨道", CATEGORY_TRACK))
    actions.append(ActionDef("SET_OFFSET", mw.set_sync_offset,
        [ParamDef("index", int), ParamDef("offset_ms", int)], None, "设置轨道偏移 (毫秒)", CATEGORY_TRACK))
    actions.append(ActionDef("SWAP_TRACKS", mw._track_manager.swap_sources,
        [ParamDef("index1", int), ParamDef("index2", int)], None, "交换轨道", CATEGORY_TRACK))
    actions.append(ActionDef("CLEAR_TRACKS", mw._track_manager.clear, [],
        None, "清空所有轨道", CATEGORY_TRACK))

    # 视图控制
    from player.ui.viewport import ViewMode
    actions.append(ActionDef("SET_VIEW_MODE", lambda mode: mw.set_view_mode(ViewMode[mode]),
        [ParamDef("mode", str)], None, "设置视图模式 (SINGLE, SIDE_BY_SIDE, COMPARISON)", CATEGORY_VIEW))
    actions.append(ActionDef("TOGGLE_FULLSCREEN", _toggle_fullscreen(mw), [],
        None, "切换全屏", CATEGORY_VIEW))
    actions.append(ActionDef("NEW_WINDOW", mw._on_new_window, [],
        None, "新建窗口", CATEGORY_VIEW))

    # 调试/诊断
    actions.append(ActionDef("TOGGLE_MEMORY_WINDOW", mw._show_memory_window, [],
        None, "切换内存监控窗口", CATEGORY_DEBUG))
    actions.append(ActionDef("TOGGLE_STATS", mw._toggle_stats_overlay, [],
        None, "切换性能统计窗口", CATEGORY_DEBUG))
    actions.append(ActionDef("SCREENSHOT", _save_screenshot(mw),
        [ParamDef("save_path", str, default="")], resolve_save_path, "保存当前帧截图", CATEGORY_DEBUG))

    # 断言动作
    actions.append(ActionDef("ASSERT_PLAYING", _assert_playing(mw), [],
        None, "断言正在播放", CATEGORY_ASSERT))
    actions.append(ActionDef("ASSERT_PAUSED", _assert_paused(mw), [],
        None, "断言已暂停", CATEGORY_ASSERT))
    actions.append(ActionDef("ASSERT_POSITION", _assert_position(mw),
        [ParamDef("expected_ms", int), ParamDef("tolerance_ms", int, default=100)],
        None, "断言播放位置", CATEGORY_ASSERT))
    actions.append(ActionDef("ASSERT_TRACK_COUNT", _assert_track_count(mw),
        [ParamDef("expected", int)], None, "断言轨道数量", CATEGORY_ASSERT))

    return actions


# 辅助函数

def _quit_app(exit_code: int):
    from PySide6.QtWidgets import QApplication
    QApplication.instance().quit()


def _toggle_play_pause(mw):
    def toggle():
        if mw._playback_controller.is_playing:
            mw.pause()
        else:
            mw.play()
    return toggle


def _speed_up(mw):
    def speed_up():
        idx = mw.controls_bar.speed_combo.currentIndex()
        if idx < mw.controls_bar.speed_combo.count() - 1:
            mw.controls_bar.speed_combo.setCurrentIndex(idx + 1)
    return speed_up


def _speed_down(mw):
    def speed_down():
        idx = mw.controls_bar.speed_combo.currentIndex()
        if idx > 0:
            mw.controls_bar.speed_combo.setCurrentIndex(idx - 1)
    return speed_down


def _zoom_in(mw):
    def zoom_in():
        idx = mw.controls_bar.zoom_combo.currentIndex()
        if idx < mw.controls_bar.zoom_combo.count() - 1:
            mw.controls_bar.zoom_combo.setCurrentIndex(idx + 1)
    return zoom_in


def _zoom_out(mw):
    def zoom_out():
        idx = mw.controls_bar.zoom_combo.currentIndex()
        if idx > 0:
            mw.controls_bar.zoom_combo.setCurrentIndex(idx - 1)
    return zoom_out


def _toggle_fullscreen(mw):
    def toggle():
        if mw.isFullScreen():
            mw.showNormal()
        else:
            mw.showFullScreen()
    return toggle


def _save_screenshot(mw):
    def save_screenshot(save_path: str = ""):
        from player.core.logging_config import get_logger
        get_logger().warning(f"SCREENSHOT action not implemented yet: {save_path}")
    return save_screenshot


class AssertionError(Exception):
    """断言失败异常"""
    pass


def _assert_playing(mw):
    def assert_playing():
        if not mw._playback_controller.is_playing:
            raise AssertionError("Expected playing, but paused")
    return assert_playing


def _assert_paused(mw):
    def assert_paused():
        if mw._playback_controller.is_playing:
            raise AssertionError("Expected paused, but playing")
    return assert_paused


def _assert_position(mw):
    def assert_position(expected_ms: int, tolerance_ms: int = 100):
        actual = mw._decoder_pool.position_ms
        diff = abs(actual - expected_ms)
        if diff > tolerance_ms:
            raise AssertionError(
                f"Position mismatch: expected {expected_ms}ms, got {actual}ms (diff={diff}ms)"
            )
    return assert_position


def _assert_track_count(mw):
    def assert_track_count(expected: int):
        actual = mw._track_manager.count()
        if actual != expected:
            raise AssertionError(f"Track count mismatch: expected {expected}, got {actual}")
    return assert_track_count
