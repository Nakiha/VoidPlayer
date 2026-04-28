# Action 系统维护指南

## 修改流程

### 新增 Action

1. 在 `lib/actions/player_action.dart` 中添加新的 sealed subclass
2. 在 `lib/windows/main/main_window_actions.dart` 中通过 `MainWindowActionBinder` 绑定 callback
3. 如需快捷键，在构造函数中传入 `LogicalKeyboardKey`，并完成下面的「新增快捷键」步骤
4. 如需自动化脚本触发，在 `lib/actions/test_runner.dart` 中补指令解析
5. 更新本文档的 Action 清单

### 新增快捷键

快捷键会自动显示在设置窗口中，只需修改以下三处（设置窗口代码无需改动）：

1. `lib/actions/player_action.dart` — 在 `shortcutEntries` 静态列表追加一行 `(labelKey: 'actionXxx', shortcutLabel: '按键名')`
2. `lib/l10n/app_en.arb` / `app_zh.arb` — 添加对应的 l10n 条目（key 与 `labelKey` 一致）
3. `lib/l10n/action_labels.dart` — `resolveActionLabel()` 的 switch 中加一个 case

### 新增 Assert

1. 在 `lib/actions/player_assert.dart` 中添加新的 sealed subclass
2. 在 `TestRunner` 中添加对应的执行分支
3. 更新 [ACTION_DESIGN.md](ACTION_DESIGN.md) 的指令列表

### 修改快捷键

直接修改 `PlayerAction` 构造函数中的 `LogicalKeyboardKey` 参数。`ActionRegistry.bind()` 会自动更新 `_keyMap` 反向索引。如果快捷键的显示名也变了，同步修改 `shortcutEntries` 中的 `shortcutLabel`。

### 移除 Action

1. 删除 sealed subclass
2. 移除 `MainWindowActionBinder.bind()` 中对应的绑定
3. 移除对应的 UI 触发点
4. 如有快捷键，移除 `shortcutEntries` 中的对应条目、`.arb` 中的 l10n 条目、`action_labels.dart` 中的 case

## 注意事项

### 绑定生命周期

主窗口 Action 由 `MainWindowActionCoordinator` 持有生命周期：

- `MainWindowController.start()` 调用 `actionCoordinator.bind()`
- `MainWindowController.dispose()` 调用 `actionCoordinator.dispose()`
- `MainWindowActionBinder` 记录自己绑定过的 action name，并在 unbind 时逐个释放

不要在 widget build 或临时对象里直接绑定全局 action。新增绑定必须保证有对应 unbind。

### 按键冲突

同一个 `LogicalKeyboardKey` 只能绑定到一个 Action。`ActionRegistry` 的 `_keyMap` 是一对一映射，后绑定的会覆盖先绑定的。

### EditableText 穿透

`ActionFocus` 默认在焦点位于 `EditableText`（`TextField`、`TextFormField` 等）时放行所有按键。如果未来有输入控件需要部分拦截，需要修改 `handleKey` 中的判断逻辑。

### 多窗口注意事项

设置窗口等子窗口由 `desktop_multi_window` 创建，运行在独立 Dart isolate 中，无法访问主窗口的 `actionRegistry`。快捷键显示使用 `PlayerAction.shortcutEntries` 静态列表，不依赖运行时注册状态。

另外，`window_manager` 插件的全局 method-channel 指针在多引擎场景下会冲突，因此子窗口不应注册该插件（见 `flutter_window.cpp` 中的 `DesktopMultiWindowSetWindowCreatedCallback`）。

### 测试脚本

- 脚本编码：UTF-8
- 时间单位：秒（浮点数）
- PTS 单位：微秒（整数）
- 注释：`#` 开头的行
- 空行：忽略
- 测试退出码：`QUIT` 指令的参数，0 表示通过
- 推荐入口：`python dev.py ui-test ui_tests/smoke/basic.csv`
- 自动化脚本优先使用 `ADD_MEDIA`，不要用会弹系统对话框的 `OPEN_FILE`
- timeline/seek 回归优先使用 `CLICK_TIMELINE_FRACTION` 覆盖真实 slider pointer 路径；`SEEK_TO` 只覆盖直接调用 seek action 的路径。

## Action 清单

> 新增/删除 Action 后更新此表。

| Action | 快捷键 | 说明 |
|--------|--------|------|
| `TOGGLE_PLAY_PAUSE` | Space | 播放/暂停 |
| `PLAY` | — | 播放 |
| `PAUSE` | — | 暂停 |
| `SEEK_TO` | — | 跳转到指定 PTS（μs） |
| `CLICK_TIMELINE_FRACTION` | — | 按比例点击 controls bar 的 timeline slider，走真实 pointer/onSeek 路径 |
| `DRAG_LOOP_HANDLE` | — | 测试脚本专用：拖动循环区间 start/end handle，走真实 pointer/onRangeChanged/onRangeChangeEnd 路径 |
| `SET_SPEED` | — | 设置倍速 |
| `STEP_FORWARD` | → | 逐帧前进 |
| `STEP_BACKWARD` | ← | 逐帧后退 |
| `OPEN_FILE` | O | 打开文件 |
| `ADD_MEDIA` | — | 按路径添加媒体 |
| `REMOVE_TRACK` | — | 按 fileId 移除轨道 |
| `TOGGLE_LAYOUT_MODE` | M | 切换布局模式 |
| `SET_LAYOUT_MODE` | — | 设置布局模式（0=并排, 1=分屏） |
| `SET_ZOOM` | — | 设置缩放比例 |
| `SET_SPLIT_POS` | — | 设置分屏位置（0.0–1.0） |
| `PAN` | — | 视口平移 |
| `SET_RENDER_SIZE` | — | 测试脚本专用：设置 renderer 输出尺寸 |
| `CAPTURE_VIEWPORT` | — | 测试脚本专用：抓取 viewport hash / 截图 |
| `WINDOW_MAXIMIZE` | — | 测试脚本专用：最大化主窗口 |
| `WINDOW_RESTORE` | — | 测试脚本专用：恢复主窗口 |
| `STORE_VIEW_CENTER` | — | 测试脚本专用：记录归一化视图中心基线 |
| `STORE_RESOURCE_USAGE` | — | 测试脚本专用：记录进程 RSS / 专用显存基线 |
| `STORE_NATIVE_SEEK_COUNT` | — | 测试脚本专用：记录当前 native 插件 seek 日志计数 |
| `NEW_WINDOW` | N | 新建窗口 |
| `OPEN_SETTINGS` | — | 打开设置窗口 |
| `OPEN_STATS` | — | 打开统计窗口 |
| `OPEN_MEMORY` | — | 打开内存窗口 |
| `RUN_ANALYSIS` | — | 触发 analysis 流程 |

## Assert 清单

> 新增/删除 Assert 后更新此表。

| Assert | 参数 | 说明 |
|--------|------|------|
| `ASSERT_PLAYING` | — | 断言正在播放 |
| `ASSERT_PAUSED` | — | 断言已暂停 |
| `ASSERT_POSITION` | ptsUs, toleranceMs | 断言播放位置 |
| `ASSERT_TRACK_COUNT` | count | 断言轨道数量 |
| `ASSERT_DURATION` | ptsUs, toleranceMs | 断言总时长 |
| `ASSERT_LAYOUT_MODE` | mode | 断言布局模式 |
| `ASSERT_ZOOM` | ratio, tolerance | 断言缩放比例 |
| `ASSERT_SPLIT_POS` | position, tolerance | 断言分屏分割线位置 |
| `ASSERT_VIEW_OFFSET` | x, y, tolerance | 断言视口平移偏移 |
| `ASSERT_VIEW_CENTER_STABLE` | baseline, tolerance | 断言当前归一化视图中心与 `STORE_VIEW_CENTER` 基线一致 |
| `ASSERT_CAPTURE_EQUALS` | expected, actual | 断言两次 viewport 截图 hash 相同 |
| `ASSERT_CAPTURE_CHANGED` | before, after | 断言两次 viewport 截图 hash 不同 |
| `ASSERT_CAPTURE_HASH` | capture, hash | 断言截图 hash |
| `ASSERT_CAPTURE_NOT_BLACK` | capture, minNonBlackRatio?, minAvgLuma? | 断言截图不是黑帧 |
| `ASSERT_TRACK_BUFFER_COUNT_BELOW` | maxCount | 断言所有轨道当前缓存帧数不超过阈值 |
| `ASSERT_RESOURCE_USAGE_BELOW` | maxRssMb, maxDedicatedGpuMb | 断言当前进程 RSS / 专用显存不超过阈值 |
| `ASSERT_RESOURCE_USAGE_DELTA_BELOW` | baseline, maxRssDeltaMb, maxDedicatedGpuDeltaMb | 断言相对 `STORE_RESOURCE_USAGE` 基线的 RSS / 专用显存增量不超过阈值 |
| `ASSERT_NATIVE_SEEK_COUNT_DELTA` | baseline, expectedDelta | 断言相对 `STORE_NATIVE_SEEK_COUNT` 基线的 native 插件 seek 次数增量 |

## 文件清单

| 文件 | 职责 |
|------|------|
| [ACTION_DESIGN.md](ACTION_DESIGN.md) | 类型体系、组件设计、脚本格式 |
| [ACTION_MAINTENANCE.md](ACTION_MAINTENANCE.md) | 本文档：修改流程、注意事项、清单 |
| `lib/actions/player_action.dart` | PlayerAction sealed class + `shortcutEntries` 显示列表 |
| `lib/actions/player_assert.dart` | PlayerAssert sealed class |
| `lib/actions/action_registry.dart` | ActionRegistry + ActionFocus |
| `lib/actions/test_runner.dart` | 脚本解析 + TestRunner |
| `lib/windows/main/main_window_actions.dart` | 主窗口 Action 绑定表和 bind/unbind 生命周期 |
| `lib/l10n/action_labels.dart` | `resolveActionLabel()` — labelKey → l10n 映射 |
