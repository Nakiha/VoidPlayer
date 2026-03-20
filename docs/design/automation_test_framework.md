# VoidPlayer 统一动作系统设计

## 概述

本文档描述 VoidPlayer 的**统一动作系统**设计，将快捷键触发、自动化测试脚本触发整合到同一套动作抽象中。核心思想：**所有操作都是 Action，只是触发源和参数来源不同**。

## 设计原则

1. **单一入口**: 每种操作只有一个实现入口，避免散落在各处
2. **参数可注入**: 动作参数可以来自快捷键(硬编码)、UI交互(用户输入)、脚本(预定义)
3. **非侵入式**: 不修改现有 Signal/Slot 机制
4. **可扩展**: 易于添加新动作

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           触发源 (Trigger Sources)                       │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                   │
│  │   Shortcut   │  │    Mock      │  │  UI Widget   │                   │
│  │   (快捷键)    │  │   (脚本)     │  │  (按钮点击)   │                   │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘                   │
│         │                 │                 │                            │
│         │  无参数或        │  完整参数       │  参数来自                  │
│         │  使用默认参数    │                 │  用户交互                  │
│         ▼                 ▼                 ▼                            │
├─────────────────────────────────────────────────────────────────────────┤
│                        ActionDispatcher (动作分发器)                     │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │ dispatch(action_name, *args, **kwargs)                          │    │
│  │ - 参数校验                                                       │    │
│  │ - 参数补全 (缺失参数时调用 resolver)                              │    │
│  │ - 执行动作                                                       │    │
│  └─────────────────────────────────────────────────────────────────┘    │
└───────────────────────────────────┬─────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                          ActionRegistry (动作注册表)                      │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │ PLAY_PAUSE:  Action(fn=toggle_play_pause, params=[])              │ │
│  │ SEEK_TO:     Action(fn=seek_to, params=[timestamp_ms])            │ │
│  │ ADD_TRACK:   Action(fn=add_media, params=[file_path])             │ │
│  │ ADD_TRACK_PROMPT: Action(fn=add_media, params=[],                 │ │
│  │                       resolver=show_file_picker)  # 交互式版本     │ │
│  └────────────────────────────────────────────────────────────────────┘ │
└───────────────────────────────────┬─────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                          MainWindow API                                  │
│  play(), pause(), seek_to(), add_media(), ...                           │
└─────────────────────────────────────────────────────────────────────────┘
```

## 动作定义模型

### Action 数据结构

```python
@dataclass
class ActionDef:
    """动作定义"""
    name: str                    # 动作名称 (大写下划线)
    fn: Callable                 # 执行函数
    params: list[ParamDef]       # 参数定义
    description: str = ""        # 描述 (用于帮助文档)
    resolver: Callable = None    # 参数解析器 (用于交互式获取参数)

@dataclass
class ParamDef:
    """参数定义"""
    name: str                    # 参数名
    type: type                   # 参数类型 (int, str, float, ...)
    default: Any = MISSING       # 默认值 (MISSING 表示必须提供)
    validator: Callable = None   # 校验函数
```

### 动作分类

| 类别 | 特点 | 示例 |
|------|------|------|
| **无参数动作** | 直接执行 | `PLAY`, `PAUSE`, `PLAY_PAUSE` |
| **有参数动作** | 需要参数 | `SEEK_TO`, `SET_SPEED` |
| **交互式动作** | 缺少参数时弹出 UI | `ADD_TRACK` (脚本传参 vs 快捷键弹文件选择器) |

### 交互式动作的两种触发方式

```python
# 动作定义
ADD_TRACK = ActionDef(
    name="ADD_TRACK",
    fn=add_media,
    params=[ParamDef("file_path", str)],
    resolver=show_file_picker,  # 交互式获取参数
)

# 触发方式 1: 脚本触发 (参数完整)
dispatcher.dispatch("ADD_TRACK", file_path="D:/test.mp4")
# -> 直接执行 add_media("D:/test.mp4")

# 触发方式 2: 快捷键触发 (无参数)
dispatcher.dispatch("ADD_TRACK")
# -> 参数缺失，调用 resolver=show_file_picker()
# -> 用户选择文件后执行 add_media(selected_path)
```

## 动作注册表

### 基础动作

| 动作名称 | 参数 | Resolver | 描述 |
|----------|------|----------|------|
| `WAIT` | `duration_ms: int` | - | 等待指定时间 |
| `QUIT` | `exit_code: int = 0` | - | 退出程序 |

### 播放控制

| 动作名称 | 参数 | Resolver | 描述 |
|----------|------|----------|------|
| `PLAY` | - | - | 开始播放 |
| `PAUSE` | - | - | 暂停播放 |
| `PLAY_PAUSE` | - | - | 切换播放/暂停 |
| `STOP` | - | - | 停止播放 |
| `SEEK_TO` | `timestamp_ms: int` | - | Seek 到指定时间点 |
| `SEEK_FORWARD` | `delta_ms: int = 5000` | - | 前进 |
| `SEEK_BACKWARD` | `delta_ms: int = 5000` | - | 后退 |
| `SEEK_RELATIVE` | `delta_ms: int` | - | 相对 Seek |
| `PREV_FRAME` | - | - | 上一帧 |
| `NEXT_FRAME` | - | - | 下一帧 |
| `TOGGLE_LOOP` | - | - | 切换循环 |

### 速度/缩放

| 动作名称 | 参数 | Resolver | 描述 |
|----------|------|----------|------|
| `SPEED_UP` | - | - | 加速 |
| `SPEED_DOWN` | - | - | 减速 |
| `SPEED_SET` | `index: int` | - | 设置速度索引 |
| `ZOOM_IN` | - | - | 放大 |
| `ZOOM_OUT` | - | - | 缩小 |
| `ZOOM_SET` | `index: int` | - | 设置缩放索引 |

### 轨道管理

| 动作名称 | 参数 | Resolver | 描述 |
|----------|------|----------|------|
| `ADD_TRACK` | `file_path: str` | `show_file_picker` | 添加媒体轨道 |
| `ADD_TRACKS` | `file_paths: list[str]` | `show_multi_file_picker` | 添加多个轨道 |
| `REMOVE_TRACK` | `index: int` | - | 移除指定轨道 |
| `SET_OFFSET` | `index: int, offset_ms: int` | - | 设置轨道偏移 |
| `SWAP_TRACKS` | `index1: int, index2: int` | - | 交换轨道 |
| `CLEAR_TRACKS` | - | - | 清空所有轨道 |

### 视图控制

| 动作名称 | 参数 | Resolver | 描述 |
|----------|------|----------|------|
| `SET_VIEW_MODE` | `mode: str` | - | 设置视图模式 |
| `TOGGLE_FULLSCREEN` | - | - | 切换全屏 |
| `NEW_WINDOW` | - | - | 新建窗口 |

### 调试/诊断

| 动作名称 | 参数 | Resolver | 描述 |
|----------|------|----------|------|
| `TOGGLE_DEBUG_MONITOR` | - | - | 切换调试监控窗口 |
| `TOGGLE_STATS` | - | - | 切换性能统计窗口 |
| `SCREENSHOT` | `save_path: str` | `show_save_dialog` | 保存当前帧截图 |

### 断言动作 (仅用于测试)

| 动作名称 | 参数 | 描述 |
|----------|------|------|
| `ASSERT_PLAYING` | - | 断言正在播放 |
| `ASSERT_PAUSED` | - | 断言已暂停 |
| `ASSERT_POSITION` | `expected_ms: int, tolerance_ms: int = 100` | 断言播放位置 |
| `ASSERT_TRACK_COUNT` | `expected: int` | 断言轨道数量 |

## Mock 脚本格式

### 文件格式

- 扩展名: `.vpmock`
- 编码: UTF-8
- 语法: `时间偏移, 动作名称[, 参数1, 参数2, ...]`

### 示例脚本

```csv
# 基础播放测试
0.5, WAIT, 500
1.0, PLAY
3.0, PAUSE
3.5, SEEK_TO, 5000
4.0, PLAY

# 添加轨道 (参数完整，不弹窗)
5.0, ADD_TRACK, D:\test\sample.mp4

# 切换视图
6.0, SET_VIEW_MODE, SIDE_BY_SIDE

# 断言验证 (仅测试时有效)
10.0, ASSERT_POSITION, 8000, 200
10.1, ASSERT_PLAYING

# 退出
15.0, QUIT, 0
```

## 快捷键绑定

快捷键系统现在只是 ActionDispatcher 的一个触发源：

```python
# player/core/shortcuts.py (重构后)

class ShortcutManager:
    """快捷键管理器 - 作为 ActionDispatcher 的触发源"""

    BINDINGS = {
        "Space": ("PLAY_PAUSE", {}),
        "Left": ("PREV_FRAME", {}),
        "Right": ("NEXT_FRAME", {}),
        "Shift+Right": ("SEEK_FORWARD", {"delta_ms": 5000}),
        "Shift+Left": ("SEEK_BACKWARD", {"delta_ms": 5000}),
        "L": ("TOGGLE_LOOP", {}),
        "F": ("TOGGLE_FULLSCREEN", {}),
        "]": ("SPEED_UP", {}),
        "[": ("SPEED_DOWN", {}),
        "Ctrl+O": ("ADD_TRACK", {}),  # 无参数，触发 resolver
        "Ctrl+N": ("NEW_WINDOW", {}),
        "I": ("TOGGLE_STATS", {}),
    }

    def _on_shortcut_activated(self, action_name: str, default_params: dict):
        # 直接转发给 ActionDispatcher
        self.action_dispatcher.dispatch(action_name, **default_params)
```

## ActionDispatcher 实现

```python
# player/core/action_dispatcher.py

class ActionDispatcher:
    """动作分发器 - 统一的命令执行入口"""

    def __init__(self, main_window: "MainWindow"):
        self._mw = main_window
        self._registry: dict[str, ActionDef] = {}
        self._register_builtin_actions()

    def dispatch(self, action_name: str, *args, **kwargs):
        """
        分发并执行动作

        1. 查找动作定义
        2. 合并参数 (传入参数 + 默认参数)
        3. 缺少必要参数时调用 resolver
        4. 校验参数
        5. 执行动作
        """
        action = self._registry.get(action_name)
        if not action:
            raise ValueError(f"Unknown action: {action_name}")

        # 合并参数
        params = self._merge_params(action, args, kwargs)

        # 缺少必要参数时尝试 resolver
        missing = self._get_missing_params(action, params)
        if missing and action.resolver:
            resolved = action.resolver(self._mw)
            if resolved is None:  # 用户取消
                return
            params.update(resolved)

        # 仍然缺少必要参数 -> 错误
        missing = self._get_missing_params(action, params)
        if missing:
            raise ValueError(f"Missing required params: {missing}")

        # 校验参数
        self._validate_params(action, params)

        # 执行
        return action.fn(**params)

    def register(self, action: ActionDef):
        """注册动作"""
        self._registry[action.name] = action

    def get_action_names(self) -> list[str]:
        """获取所有动作名称"""
        return list(self._registry.keys())
```

## 命令行接口

```bash
# 运行自动化测试脚本
python run_player.py --mock tests/mock/basic_playback.vpmock

# 指定初始文件 + mock 脚本
python run_player.py -i video1.mp4 -i video2.mp4 --mock tests/mock/compare.vpmock

# CI 模式
python run_player.py --mock tests/mock/ci_test.vpmock --ci

# 列出所有可用动作
python run_player.py --list-actions
```

## 代码结构

```
player/
├── core/
│   ├── action_dispatcher.py    # 动作分发器
│   ├── action_registry.py      # 动作定义和注册
│   ├── action_resolvers.py     # 交互式参数解析器 (文件选择器等)
│   ├── shortcuts.py            # 快捷键管理 (触发源之一)
│   ├── automation.py           # 自动化控制器 (触发源之一)
│   └── ...
└── ui/
    └── main_window.py

tests/
├── mock/
│   ├── basic_playback.vpmock
│   ├── multi_track.vpmock
│   └── stress_test.vpmock
└── test_actions.py
```

## 实现计划

### Phase 1: 核心框架

1. **ActionDef / ParamDef** 数据结构
2. **ActionDispatcher** 实现
3. **注册内置动作**

### Phase 2: 整合快捷键

1. 重构 `ShortcutManager` 为触发源
2. 移除 `ShortcutAction` 枚举 (用动作名称字符串替代)
3. 快捷键绑定改为 `(action_name, default_params)` 格式

### Phase 3: 自动化测试

1. **AutomationController** 脚本解析和执行
2. 命令行 `--mock` 参数支持
3. 断言动作实现
4. 示例测试脚本

### Phase 4: 验证与报告

1. 动作执行日志
2. 断言结果记录
3. 测试报告生成

## 与现有系统的关系

```
┌─────────────────────────────────────────────────────────┐
│                     现有系统                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐     │
│  │ SignalBus   │  │ Shortcuts   │  │ UI Callbacks│     │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘     │
│         │                │                │             │
│         │    保持不变    │   重构为       │  可选重构   │
│         │                │   触发源       │             │
│         ▼                ▼                ▼             │
│  ┌─────────────────────────────────────────────────┐   │
│  │              新增: ActionDispatcher              │   │
│  └─────────────────────────┬───────────────────────┘   │
│                            │                           │
│                            ▼                           │
│  ┌─────────────────────────────────────────────────┐   │
│  │              MainWindow API (不变)               │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

**关键点**:
- SignalBus 保持不变，继续用于组件间通信
- ShortcutManager 重构为 ActionDispatcher 的触发源
- UI 回调可以选择直接调用 ActionDispatcher 或保持现有方式

## 扩展方向

1. **录制模式**: 记录用户操作生成 `.vpmock` 脚本
2. **动作宏**: 组合多个动作为单一命令
3. **Python DSL**: 直接用 Python 脚本控制
4. **远程控制**: 通过 socket/HTTP 接收动作命令

---

*文档版本: 2.0*
*最后更新: 2026-03-21*
