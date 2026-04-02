# Action 系统维护指南

## 修改流程

### 新增 Action

1. 在 `lib/actions/player_action.dart` 中添加新的 sealed subclass
2. 在 `main.dart`（或对应初始化位置）调用 `actionRegistry.register()`
3. 在 UI 控件的 `initState` 中 `bind`，`dispose` 中 `unbind`
4. 如需快捷键，在构造函数中传入 `LogicalKeyboardKey`
5. 更新本文档的 [Action 清单](#action-清单)

### 新增 Assert

1. 在 `lib/actions/player_assert.dart` 中添加新的 sealed subclass
2. 在 `TestRunner` 中添加对应的执行分支
3. 更新 [ACTION_DESIGN.md](ACTION_DESIGN.md) 的指令列表

### 修改快捷键

直接修改 `PlayerAction` 构造函数中的 `defaultShortcut` 参数。注册表会在 `register` 时自动更新 `_keyMap` 反向索引。无需修改其他代码。

### 移除 Action

1. 删除 sealed subclass
2. 移除所有 `bind` / `unbind` 调用
3. 移除对应的 UI 触发点
4. 更新文档清单

## 注意事项

### 按键冲突

同一个 `LogicalKeyboardKey` 只能绑定到一个 Action。如果注册时发现冲突，`register` 应抛出 `ArgumentError`。

### EditableText 穿透

`ActionFocus` 默认在焦点位于 `EditableText`（`TextField`、`TextFormField` 等）时放行所有按键。如果未来有输入控件需要部分拦截，需要修改 `handleKey` 中的判断逻辑。

### 弹窗中控件

弹窗内的控件（如 `PageUp`/`PageDown` 滚动列表）遵循同样的懒绑定模式：

```dart
// 弹窗 widget
@override
void initState() {
  super.initState();
  actionRegistry.bind('PAGE_DOWN', _onPageDown);
}

@override
void dispose() {
  actionRegistry.unbind('PAGE_DOWN');
  super.dispose();
}
```

控件未渲染时 callback 为 null，按键不会被拦截，也不会报错。

### 测试脚本

- 脚本编码：UTF-8
- 时间单位：秒（浮点数）
- PTS 单位：微秒（整数）
- 注释：`#` 开头的行
- 空行：忽略
- 测试退出码：`QUIT` 指令的参数，0 表示通过

## Action 清单

> 新增/删除 Action 后更新此表。

| Action | 快捷键 | 说明 |
|--------|--------|------|
| `PLAY` | Space | 播放 |
| `PAUSE` | — | 暂停 |
| `SEEK_TO` | — | 跳转到指定 PTS（μs） |
| `SET_SPEED` | — | 设置倍速 |
| `STEP_FORWARD` | → | 逐帧前进 |
| `STEP_BACKWARD` | ← | 逐帧后退 |
| `OPEN_FILE` | O | 打开文件 |

## Assert 清单

> 新增/删除 Assert 后更新此表。

| Assert | 参数 | 说明 |
|--------|------|------|
| `ASSERT_PLAYING` | — | 断言正在播放 |
| `ASSERT_PAUSED` | — | 断言已暂停 |
| `ASSERT_POSITION` | ptsUs, toleranceMs | 断言播放位置 |
| `ASSERT_TRACK_COUNT` | count | 断言轨道数量 |
| `ASSERT_DURATION` | ptsUs, toleranceMs | 断言总时长 |

## 文件清单

| 文件 | 职责 |
|------|------|
| [ACTION_DESIGN.md](ACTION_DESIGN.md) | 类型体系、组件设计、脚本格式 |
| [ACTION_MAINTENANCE.md](ACTION_MAINTENANCE.md) | 本文档：修改流程、注意事项、清单 |
| `lib/actions/player_action.dart` | PlayerAction sealed class |
| `lib/actions/player_assert.dart` | PlayerAssert sealed class |
| `lib/actions/action_registry.dart` | ActionRegistry + ActionFocus |
| `lib/actions/test_runner.dart` | 脚本解析 + TestRunner |
