# 快捷键规范

## 架构概览

快捷键系统采用三层架构：

```
用户按键 → ShortcutManager → ActionDispatcher → ActionRegistry → MainWindow
```

- **ShortcutManager** (`player/core/shortcuts.py`): 管理快捷键绑定和分发
- **ActionDispatcher** (`player/core/actions/dispatcher.py`): 动作分发器
- **ActionRegistry** (`player/core/actions/registry.py`): 动作定义和执行

## 新增快捷键清单

完成一个需要快捷键的功能时，按顺序修改：

### 1. ActionRegistry (必须)

文件: `player/core/actions/registry.py`

```python
# 在 get_action_metadata() 添加元数据
ActionDef("MY_ACTION", noop, [ParamDef("param", int, default=0)],
          "动作描述", CATEGORY_XXX),

# 在 create_action_registry() 添加执行逻辑
ActionDef("MY_ACTION", fn=mw.my_method,
          params=[ParamDef("param", int, default=0)],
          description="动作描述",
          category=CATEGORY_XXX),
```

### 2. ShortcutManager (可选，如需快捷键)

文件: `player/core/shortcuts.py`

```python
# 1. 添加枚举
class ShortcutAction(Enum):
    MY_ACTION = auto()

# 2. 添加分类映射 (用于设置页面显示)
SHORTCUT_CATEGORY_MAP = {
    ShortcutAction.MY_ACTION: SHORTCUT_CATEGORY_XXX,  # 选择合适分类
}

# 3. 添加动作名映射
ACTION_NAME_MAP: dict[ShortcutAction, str] = {
    ShortcutAction.MY_ACTION: "MY_ACTION",
}

# 4. 添加绑定 (快捷键, 描述, 默认参数)
DEFAULT_BINDINGS: dict[ShortcutAction, tuple[str, str, dict]] = {
    ShortcutAction.MY_ACTION: ("Ctrl+M", "我的动作", {"param": 0}),
}
```

**可用分类**:
- `SHORTCUT_CATEGORY_PLAYBACK` - 播放控制
- `SHORTCUT_CATEGORY_SPEED` - 速度控制
- `SHORTCUT_CATEGORY_ZOOM` - 缩放控制
- `SHORTCUT_CATEGORY_PROJECT` - 项目操作
- `SHORTCUT_CATEGORY_OTHER` - 其他

### 3. Mock 测试 (推荐)

文件: `tests/mock/xxx.vpmock`

```csv
0.5, MY_ACTION, 10
2.0, QUIT, 0
```

## 当前快捷键绑定

### 播放控制
| 快捷键 | 动作 | 说明 |
|--------|------|------|
| `Space` | PLAY_PAUSE | 播放/暂停 |
| `Left` | PREV_FRAME | 上一帧 |
| `Right` | NEXT_FRAME | 下一帧 |
| `Shift+Left` | SEEK_BACKWARD | 后退 5 秒 |
| `Shift+Right` | SEEK_FORWARD | 前进 5 秒 |
| `L` | TOGGLE_LOOP | 切换循环 |
| `F` | TOGGLE_FULLSCREEN | 全屏 |

### 速度控制
| 快捷键 | 动作 | 说明 |
|--------|------|------|
| `]` | SPEED_UP | 加速 |
| `[` | SPEED_DOWN | 减速 |
| `\` | SPEED_RESET | 重置速度 |

### 缩放控制
| 快捷键 | 动作 | 说明 |
|--------|------|------|
| `Ctrl++` | ZOOM_IN | 放大 |
| `Ctrl+-` | ZOOM_OUT | 缩小 |
| `Ctrl+0` | ZOOM_RESET | 重置缩放 (Fit) |

### 项目操作
| 快捷键 | 动作 | 说明 |
|--------|------|------|
| `Ctrl+O` | ADD_TRACK | 添加媒体 |
| `Ctrl+N` | NEW_WINDOW | 新窗口 |
| `Ctrl+Shift+O` | OPEN_PROJECT | 打开项目 |
| `Ctrl+S` | SAVE_PROJECT | 保存项目 |

### 调试
| 快捷键 | 动作 | 说明 |
|--------|------|------|
| `Ctrl+D` | TOGGLE_MEMORY_WINDOW | 内存监控 |
| `I` | TOGGLE_STATS | 性能统计 |

## 快捷键设计原则

1. **避免冲突**: 添加前检查现有绑定
2. **一致性**: 类似功能使用类似快捷键 (如 Ctrl+O 打开)
3. **可发现性**: 常用功能用单键，高级功能用组合键
4. **跨平台**: 避免平台特定键 (如 Cmd 仅 macOS)

## 防抖机制

ShortcutManager 内置 150ms 防抖，防止按键重复触发。
