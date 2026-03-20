"""
ActionRegistry - 动作注册表

定义所有内置动作，提供工厂方法创建 ActionDispatcher 并注册动作
单一数据源：所有动作定义、参数、描述、分类都在此文件中
"""
from typing import TYPE_CHECKING

from .action_types import (
    ActionDef, ParamDef,
    CATEGORY_BASIC, CATEGORY_PLAYBACK, CATEGORY_SPEED_ZOOM,
    CATEGORY_TRACK, CATEGORY_VIEW, CATEGORY_DEBUG, CATEGORY_ASSERT,
)
from .action_resolvers import (
    resolve_file_picker,
    resolve_multi_file_picker,
    resolve_save_path,
)

if TYPE_CHECKING:
    from ..ui.main_window import MainWindow


# ========== 元数据定义 (用于 --list-actions) ==========

def get_action_metadata() -> list[ActionDef]:
    """
    获取动作元数据列表 (不绑定 MainWindow)

    用于 --list-actions 显示帮助信息，fn 字段为占位函数

    Returns:
        动作定义列表 (仅元数据有效，fn 为空实现)
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


# ========== 完整动作注册表 (绑定 MainWindow) ==========

def create_action_registry(mw: "MainWindow") -> list[ActionDef]:
    """
    创建动作注册表

    Args:
        mw: MainWindow 实例，用于绑定动作执行函数

    Returns:
        动作定义列表
    """
    actions = []

    # ========== 基础动作 ==========

    actions.append(ActionDef(
        name="WAIT",
        fn=lambda duration_ms: None,  # WAIT 在 AutomationController 中特殊处理
        params=[ParamDef("duration_ms", int)],
        description="等待指定时间 (毫秒)",
        category=CATEGORY_BASIC,
    ))

    actions.append(ActionDef(
        name="QUIT",
        fn=lambda exit_code=0: _quit_app(mw, exit_code),
        params=[ParamDef("exit_code", int, default=0)],
        description="退出程序",
        category=CATEGORY_BASIC,
    ))

    # ========== 播放控制 ==========

    actions.append(ActionDef(
        name="PLAY",
        fn=mw.play,
        params=[],
        description="开始播放",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="PAUSE",
        fn=mw.pause,
        params=[],
        description="暂停播放",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="PLAY_PAUSE",
        fn=_toggle_play_pause(mw),
        params=[],
        description="切换播放/暂停",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="STOP",
        fn=lambda: (mw.pause(), mw.seek_to(0)),
        params=[],
        description="停止播放并回到开头",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="SEEK_TO",
        fn=mw.seek_to,
        params=[ParamDef("timestamp_ms", int)],
        description="Seek 到指定时间点 (毫秒)",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="SEEK_FORWARD",
        fn=lambda delta_ms=5000: mw.seek_to(mw._decoder_pool.position_ms + delta_ms),
        params=[ParamDef("delta_ms", int, default=5000)],
        description="前进 (毫秒)",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="SEEK_BACKWARD",
        fn=lambda delta_ms=5000: mw.seek_to(max(0, mw._decoder_pool.position_ms - delta_ms)),
        params=[ParamDef("delta_ms", int, default=5000)],
        description="后退 (毫秒)",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="SEEK_RELATIVE",
        fn=lambda delta_ms: mw.seek_to(max(0, mw._decoder_pool.position_ms + delta_ms)),
        params=[ParamDef("delta_ms", int)],
        description="相对 Seek (毫秒，正数前进，负数后退)",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="PREV_FRAME",
        fn=mw._decoder_pool.prev_frame,
        params=[],
        description="上一帧",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="NEXT_FRAME",
        fn=mw._decoder_pool.next_frame,
        params=[],
        description="下一帧",
        category=CATEGORY_PLAYBACK,
    ))

    actions.append(ActionDef(
        name="TOGGLE_LOOP",
        fn=lambda: mw.controls_bar.loop_btn.toggle(),
        params=[],
        description="切换循环",
        category=CATEGORY_PLAYBACK,
    ))

    # ========== 速度/缩放 ==========

    actions.append(ActionDef(
        name="SPEED_UP",
        fn=_speed_up(mw),
        params=[],
        description="加速",
        category=CATEGORY_SPEED_ZOOM,
    ))

    actions.append(ActionDef(
        name="SPEED_DOWN",
        fn=_speed_down(mw),
        params=[],
        description="减速",
        category=CATEGORY_SPEED_ZOOM,
    ))

    actions.append(ActionDef(
        name="SPEED_SET",
        fn=lambda index: mw.controls_bar.speed_combo.setCurrentIndex(index),
        params=[ParamDef("index", int)],
        description="设置速度索引",
        category=CATEGORY_SPEED_ZOOM,
    ))

    actions.append(ActionDef(
        name="ZOOM_IN",
        fn=_zoom_in(mw),
        params=[],
        description="放大",
        category=CATEGORY_SPEED_ZOOM,
    ))

    actions.append(ActionDef(
        name="ZOOM_OUT",
        fn=_zoom_out(mw),
        params=[],
        description="缩小",
        category=CATEGORY_SPEED_ZOOM,
    ))

    actions.append(ActionDef(
        name="ZOOM_SET",
        fn=lambda index: mw.controls_bar.zoom_combo.setCurrentIndex(index),
        params=[ParamDef("index", int)],
        description="设置缩放索引",
        category=CATEGORY_SPEED_ZOOM,
    ))

    # ========== 轨道管理 ==========

    actions.append(ActionDef(
        name="ADD_TRACK",
        fn=mw.add_media,
        params=[ParamDef("file_path", str)],
        resolver=resolve_file_picker,
        description="添加媒体轨道",
        category=CATEGORY_TRACK,
    ))

    actions.append(ActionDef(
        name="ADD_TRACKS",
        fn=lambda file_paths: [mw.add_media(p) for p in file_paths],
        params=[ParamDef("file_paths", list)],
        resolver=resolve_multi_file_picker,
        description="添加多个轨道",
        category=CATEGORY_TRACK,
    ))

    actions.append(ActionDef(
        name="REMOVE_TRACK",
        fn=mw.remove_media,
        params=[ParamDef("index", int)],
        description="移除指定轨道",
        category=CATEGORY_TRACK,
    ))

    actions.append(ActionDef(
        name="SET_OFFSET",
        fn=mw.set_sync_offset,
        params=[ParamDef("index", int), ParamDef("offset_ms", int)],
        description="设置轨道偏移 (毫秒)",
        category=CATEGORY_TRACK,
    ))

    actions.append(ActionDef(
        name="SWAP_TRACKS",
        fn=mw._track_manager.swap_sources,
        params=[ParamDef("index1", int), ParamDef("index2", int)],
        description="交换轨道",
        category=CATEGORY_TRACK,
    ))

    actions.append(ActionDef(
        name="CLEAR_TRACKS",
        fn=mw._track_manager.clear,
        params=[],
        description="清空所有轨道",
        category=CATEGORY_TRACK,
    ))

    # ========== 视图控制 ==========

    from ..ui.viewport import ViewMode

    actions.append(ActionDef(
        name="SET_VIEW_MODE",
        fn=lambda mode: mw.set_view_mode(ViewMode[mode]),
        params=[ParamDef("mode", str)],
        description="设置视图模式 (SINGLE, SIDE_BY_SIDE, COMPARISON)",
        category=CATEGORY_VIEW,
    ))

    actions.append(ActionDef(
        name="TOGGLE_FULLSCREEN",
        fn=_toggle_fullscreen(mw),
        params=[],
        description="切换全屏",
        category=CATEGORY_VIEW,
    ))

    actions.append(ActionDef(
        name="NEW_WINDOW",
        fn=mw._on_new_window,
        params=[],
        description="新建窗口",
        category=CATEGORY_VIEW,
    ))

    # ========== 调试/诊断 ==========

    actions.append(ActionDef(
        name="TOGGLE_DEBUG_MONITOR",
        fn=mw._show_debug_monitor,
        params=[],
        description="切换调试监控窗口",
        category=CATEGORY_DEBUG,
    ))

    actions.append(ActionDef(
        name="TOGGLE_STATS",
        fn=mw._toggle_stats_overlay,
        params=[],
        description="切换性能统计窗口",
        category=CATEGORY_DEBUG,
    ))

    actions.append(ActionDef(
        name="SCREENSHOT",
        fn=_save_screenshot(mw),
        params=[ParamDef("save_path", str, default="")],
        resolver=resolve_save_path,
        description="保存当前帧截图",
        category=CATEGORY_DEBUG,
    ))

    # ========== 断言动作 (仅用于测试) ==========

    actions.append(ActionDef(
        name="ASSERT_PLAYING",
        fn=_assert_playing(mw),
        params=[],
        description="断言正在播放",
        category=CATEGORY_ASSERT,
    ))

    actions.append(ActionDef(
        name="ASSERT_PAUSED",
        fn=_assert_paused(mw),
        params=[],
        description="断言已暂停",
        category=CATEGORY_ASSERT,
    ))

    actions.append(ActionDef(
        name="ASSERT_POSITION",
        fn=_assert_position(mw),
        params=[
            ParamDef("expected_ms", int),
            ParamDef("tolerance_ms", int, default=100),
        ],
        description="断言播放位置",
        category=CATEGORY_ASSERT,
    ))

    actions.append(ActionDef(
        name="ASSERT_TRACK_COUNT",
        fn=_assert_track_count(mw),
        params=[ParamDef("expected", int)],
        description="断言轨道数量",
        category=CATEGORY_ASSERT,
    ))

    return actions


# ========== 辅助函数 ==========

def _quit_app(mw: "MainWindow", exit_code: int):
    """退出应用"""
    from PySide6.QtWidgets import QApplication
    QApplication.instance().quit()


def _toggle_play_pause(mw: "MainWindow"):
    def toggle():
        if mw._playback_controller.is_playing:
            mw.pause()
        else:
            mw.play()
    return toggle


def _speed_up(mw: "MainWindow"):
    def speed_up():
        idx = mw.controls_bar.speed_combo.currentIndex()
        if idx < mw.controls_bar.speed_combo.count() - 1:
            mw.controls_bar.speed_combo.setCurrentIndex(idx + 1)
    return speed_up


def _speed_down(mw: "MainWindow"):
    def speed_down():
        idx = mw.controls_bar.speed_combo.currentIndex()
        if idx > 0:
            mw.controls_bar.speed_combo.setCurrentIndex(idx - 1)
    return speed_down


def _zoom_in(mw: "MainWindow"):
    def zoom_in():
        idx = mw.controls_bar.zoom_combo.currentIndex()
        if idx < mw.controls_bar.zoom_combo.count() - 1:
            mw.controls_bar.zoom_combo.setCurrentIndex(idx + 1)
    return zoom_in


def _zoom_out(mw: "MainWindow"):
    def zoom_out():
        idx = mw.controls_bar.zoom_combo.currentIndex()
        if idx > 0:
            mw.controls_bar.zoom_combo.setCurrentIndex(idx - 1)
    return zoom_out


def _toggle_fullscreen(mw: "MainWindow"):
    def toggle():
        if mw.isFullScreen():
            mw.showNormal()
        else:
            mw.showFullScreen()
    return toggle


def _save_screenshot(mw: "MainWindow"):
    def save_screenshot(save_path: str = ""):
        # TODO: 实现截图保存
        from .logging_config import get_logger
        get_logger().warning(f"SCREENSHOT action not implemented yet: {save_path}")
    return save_screenshot


# ========== 断言辅助函数 ==========

class AssertionError(Exception):
    """断言失败异常"""
    pass


def _assert_playing(mw: "MainWindow"):
    def assert_playing():
        if not mw._playback_controller.is_playing:
            raise AssertionError("Expected playing, but paused")
    return assert_playing


def _assert_paused(mw: "MainWindow"):
    def assert_paused():
        if mw._playback_controller.is_playing:
            raise AssertionError("Expected paused, but playing")
    return assert_paused


def _assert_position(mw: "MainWindow"):
    def assert_position(expected_ms: int, tolerance_ms: int = 100):
        actual = mw._decoder_pool.position_ms
        diff = abs(actual - expected_ms)
        if diff > tolerance_ms:
            raise AssertionError(
                f"Position mismatch: expected {expected_ms}ms, got {actual}ms (diff={diff}ms, tolerance={tolerance_ms}ms)"
            )
    return assert_position


def _assert_track_count(mw: "MainWindow"):
    def assert_track_count(expected: int):
        actual = mw._track_manager.count()
        if actual != expected:
            raise AssertionError(f"Track count mismatch: expected {expected}, got {actual}")
    return assert_track_count
