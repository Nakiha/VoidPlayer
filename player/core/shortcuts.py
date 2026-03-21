"""
ShortcutManager - 全局快捷键管理
统一管理所有键盘快捷键，作为 ActionDispatcher 的触发源
"""
import time
from enum import Enum, auto
from typing import Callable, Optional, TYPE_CHECKING
from PySide6.QtCore import QObject, Qt
from PySide6.QtGui import QKeySequence, QShortcut
from PySide6.QtWidgets import QWidget, QApplication

if TYPE_CHECKING:
    from .actions import ActionDispatcher


class ShortcutAction(Enum):
    """快捷键动作枚举 - 映射到 ActionDispatcher 的动作名称"""
    # 播放控制
    PLAY_PAUSE = auto()
    PREV_FRAME = auto()
    NEXT_FRAME = auto()
    SEEK_FORWARD = auto()
    SEEK_BACKWARD = auto()
    TOGGLE_LOOP = auto()
    TOGGLE_FULLSCREEN = auto()

    # 速度控制
    SPEED_UP = auto()
    SPEED_DOWN = auto()
    SPEED_RESET = auto()

    # 缩放控制
    ZOOM_IN = auto()
    ZOOM_OUT = auto()
    ZOOM_RESET = auto()

    # 项目操作
    ADD_MEDIA = auto()
    NEW_WINDOW = auto()
    OPEN_PROJECT = auto()
    SAVE_PROJECT = auto()

    # 其他
    TOGGLE_DEBUG_MONITOR = auto()
    TOGGLE_STATS = auto()


# ShortcutAction 到 ActionDispatcher 动作名称的映射
ACTION_NAME_MAP: dict[ShortcutAction, str] = {
    ShortcutAction.PLAY_PAUSE: "PLAY_PAUSE",
    ShortcutAction.PREV_FRAME: "PREV_FRAME",
    ShortcutAction.NEXT_FRAME: "NEXT_FRAME",
    ShortcutAction.SEEK_FORWARD: "SEEK_FORWARD",
    ShortcutAction.SEEK_BACKWARD: "SEEK_BACKWARD",
    ShortcutAction.TOGGLE_LOOP: "TOGGLE_LOOP",
    ShortcutAction.TOGGLE_FULLSCREEN: "TOGGLE_FULLSCREEN",
    ShortcutAction.SPEED_UP: "SPEED_UP",
    ShortcutAction.SPEED_DOWN: "SPEED_DOWN",
    ShortcutAction.SPEED_RESET: "SPEED_SET",  # 特殊处理
    ShortcutAction.ZOOM_IN: "ZOOM_IN",
    ShortcutAction.ZOOM_OUT: "ZOOM_OUT",
    ShortcutAction.ZOOM_RESET: "ZOOM_SET",  # 特殊处理
    ShortcutAction.ADD_MEDIA: "ADD_TRACK",
    ShortcutAction.NEW_WINDOW: "NEW_WINDOW",
    ShortcutAction.OPEN_PROJECT: "OPEN_PROJECT",
    ShortcutAction.SAVE_PROJECT: "SAVE_PROJECT",
    ShortcutAction.TOGGLE_DEBUG_MONITOR: "TOGGLE_DEBUG_MONITOR",
    ShortcutAction.TOGGLE_STATS: "TOGGLE_STATS",
}


class ShortcutManager(QObject):
    """
    快捷键管理器 - 作为 ActionDispatcher 的触发源

    统一管理所有快捷键，支持：
    - 集中定义快捷键绑定
    - 避免快捷键冲突
    - 允许动态启用/禁用快捷键
    - 使用 ApplicationShortcut 上下文确保快捷键始终生效
    - 触发时通过 ActionDispatcher 分发动作
    """

    # 默认快捷键绑定: (快捷键序列, 描述, 默认参数)
    DEFAULT_BINDINGS: dict[ShortcutAction, tuple[str, str, dict]] = {
        # 播放控制
        ShortcutAction.PLAY_PAUSE: ("Space", "播放/暂停", {}),
        ShortcutAction.PREV_FRAME: ("Left", "上一帧", {}),
        ShortcutAction.NEXT_FRAME: ("Right", "下一帧", {}),
        ShortcutAction.SEEK_FORWARD: ("Shift+Right", "前进 5 秒", {"delta_ms": 5000}),
        ShortcutAction.SEEK_BACKWARD: ("Shift+Left", "后退 5 秒", {"delta_ms": 5000}),
        ShortcutAction.TOGGLE_LOOP: ("L", "切换循环", {}),
        ShortcutAction.TOGGLE_FULLSCREEN: ("F", "全屏", {}),

        # 速度控制
        ShortcutAction.SPEED_UP: ("]", "加速", {}),
        ShortcutAction.SPEED_DOWN: ("[", "减速", {}),
        ShortcutAction.SPEED_RESET: ("\\", "重置速度", {"index": 2}),  # index=2 是 1.0x

        # 缩放控制
        ShortcutAction.ZOOM_IN: ("Ctrl++", "放大", {}),
        ShortcutAction.ZOOM_OUT: ("Ctrl+-", "缩小", {}),
        ShortcutAction.ZOOM_RESET: ("Ctrl+0", "重置缩放", {"index": 2}),  # index=2 是 100%

        # 项目操作
        ShortcutAction.ADD_MEDIA: ("Ctrl+O", "添加媒体", {}),  # 无参数，触发 resolver
        ShortcutAction.NEW_WINDOW: ("Ctrl+N", "新窗口", {}),
        ShortcutAction.OPEN_PROJECT: ("Ctrl+Shift+O", "打开项目", {}),
        ShortcutAction.SAVE_PROJECT: ("Ctrl+S", "保存项目", {}),

        # 其他
        ShortcutAction.TOGGLE_DEBUG_MONITOR: ("Ctrl+D", "性能监控", {}),
        ShortcutAction.TOGGLE_STATS: ("I", "性能统计", {}),
    }

    # 防抖间隔 (秒) - 同一快捷键在此时间内只响应一次
    DEBOUNCE_INTERVAL = 0.15

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self._shortcuts: dict[ShortcutAction, QShortcut] = {}
        self._callbacks: dict[ShortcutAction, Callable] = {}  # 保留兼容性
        self._action_dispatcher: Optional["ActionDispatcher"] = None
        self._enabled: bool = True
        self._action_enabled: dict[ShortcutAction, bool] = {}
        self._last_trigger_time: dict[ShortcutAction, float] = {}  # 防抖时间戳

    def set_action_dispatcher(self, dispatcher: "ActionDispatcher"):
        """设置 ActionDispatcher (推荐方式)"""
        self._action_dispatcher = dispatcher

    def setup(self, parent_widget: QWidget):
        """
        设置所有快捷键

        Args:
            parent_widget: 快捷键的父控件 (通常是 MainWindow)
        """
        for action in ShortcutAction:
            if action in self.DEFAULT_BINDINGS:
                key_sequence, _, _ = self.DEFAULT_BINDINGS[action]
                self._create_shortcut(action, key_sequence, parent_widget)
                self._action_enabled[action] = True

    def _create_shortcut(self, action: ShortcutAction, key_sequence: str, parent: QWidget):
        """创建单个快捷键"""
        shortcut = QShortcut(QKeySequence(key_sequence), parent)
        # 使用 ApplicationShortcut 确保快捷键在控件有焦点时也能生效
        shortcut.setContext(Qt.ShortcutContext.ApplicationShortcut)
        shortcut.activated.connect(lambda: self._on_activated(action))
        self._shortcuts[action] = shortcut

    def _on_activated(self, action: ShortcutAction):
        """快捷键激活回调"""
        if not self._enabled:
            return

        if not self._action_enabled.get(action, True):
            return

        # 防抖检查 - 避免按键重复触发
        now = time.time()
        last_time = self._last_trigger_time.get(action, 0)
        if now - last_time < self.DEBOUNCE_INTERVAL:
            return
        self._last_trigger_time[action] = now

        # 优先使用 ActionDispatcher
        if self._action_dispatcher:
            self._dispatch_to_action_dispatcher(action)
            return

        # 回退到传统回调方式 (兼容性)
        callback = self._callbacks.get(action)
        if callback:
            callback()

    def _dispatch_to_action_dispatcher(self, action: ShortcutAction):
        """通过 ActionDispatcher 分发动作"""
        if not self._action_dispatcher:
            return

        action_name = ACTION_NAME_MAP.get(action)
        if not action_name:
            return

        # 获取默认参数
        binding = self.DEFAULT_BINDINGS.get(action)
        if binding:
            _, _, default_params = binding
            try:
                self._action_dispatcher.dispatch(action_name, **default_params)
            except Exception as e:
                from .logging_config import get_logger
                get_logger().error(f"Shortcut dispatch failed: {action_name} -> {e}")

    def bind(self, action: ShortcutAction, callback: Callable):
        """
        绑定快捷键回调

        Args:
            action: 快捷键动作
            callback: 回调函数
        """
        self._callbacks[action] = callback

    def unbind(self, action: ShortcutAction):
        """解除快捷键回调绑定"""
        self._callbacks.pop(action, None)

    def set_enabled(self, enabled: bool):
        """设置全局启用状态"""
        self._enabled = enabled

    def set_action_enabled(self, action: ShortcutAction, enabled: bool):
        """设置单个动作的启用状态"""
        self._action_enabled[action] = enabled

    def get_key_sequence(self, action: ShortcutAction) -> str:
        """获取快捷键序列字符串"""
        if action in self.DEFAULT_BINDINGS:
            return self.DEFAULT_BINDINGS[action][0]
        return ""

    def get_description(self, action: ShortcutAction) -> str:
        """获取快捷键描述"""
        if action in self.DEFAULT_BINDINGS:
            return self.DEFAULT_BINDINGS[action][1]
        return ""

    def get_default_params(self, action: ShortcutAction) -> dict:
        """获取快捷键默认参数"""
        if action in self.DEFAULT_BINDINGS:
            return self.DEFAULT_BINDINGS[action][2]
        return {}

    def get_all_shortcuts_info(self) -> list[tuple[str, str]]:
        """
        获取所有快捷键信息 (用于显示帮助/设置)

        Returns:
            [(快捷键, 描述), ...]
        """
        result = []
        for action in ShortcutAction:
            if action in self.DEFAULT_BINDINGS:
                key, desc, _ = self.DEFAULT_BINDINGS[action]
                result.append((key, desc))
        return result

    def clear_focus_from_input_widgets(self):
        """
        清除输入控件的焦点，确保快捷键不被拦截

        当用户点击非输入区域或触发快捷键时调用
        """
        focus_widget = QApplication.focusWidget()
        if focus_widget:
            # 检查是否是输入类控件
            from PySide6.QtWidgets import QLineEdit, QTextEdit, QComboBox, QAbstractSpinBox
            if isinstance(focus_widget, (QLineEdit, QTextEdit, QComboBox, QAbstractSpinBox)):
                focus_widget.clearFocus()
