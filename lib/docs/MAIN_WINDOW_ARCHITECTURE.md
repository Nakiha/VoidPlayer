# 主窗口架构

> 主窗口是播放器最容易累积耦合的区域。本文档固定当前边界，后续新增功能按这里的所有权放置。

## 总览

```text
MainWindow (StatefulWidget shell)
  └── MainWindowController
      ├── MainWindowStateStore
      ├── VideoRendererController
      ├── TrackManager
      ├── MainWindowPlaybackCoordinator
      ├── MainWindowLayoutCoordinator
      ├── MainWindowMediaCoordinator
      ├── MainWindowAnalysisCoordinator
      ├── MainWindowActionCoordinator
      └── MainWindowTestHarness

MainWindowView
  ├── MainWindowViewModel
  └── MainWindowViewActions
```

`main_window.dart` 只负责：

- 创建 `MainWindowController`
- 在 `dispose` 中释放 controller
- 用 `ListenableBuilder` 将 controller 的 state 变化映射到 `MainWindowView`

## 文件职责

| 文件 | 职责 |
|------|------|
| `main_window.dart` | 薄 widget shell，不放业务 |
| `main_window_controller.dart` | facade，装配和协调各 coordinator |
| `main_window_state.dart` | `MainWindowStateModel` + `MainWindowStateStore` |
| `main_window_view.dart` | 纯 view，吃 `MainWindowViewModel` 和 `MainWindowViewActions` |
| `main_window_actions.dart` | ActionRegistry 绑定/解绑生命周期 |
| `main_window_playback.dart` | play/pause/seek、polling、loop range、timeline hover |
| `main_window_layout.dart` | viewport resize debounce、pan/zoom/split、native layout flush |
| `main_window_media.dart` | open/add/remove media、track offset、effective duration |
| `main_window_analysis.dart` | analysis IPC snapshot 和 analysis window trigger |
| `main_window_test_hooks.dart` | UI 自动化专用 pointer simulation |

## 生命周期

```text
MainWindow.initState
  → MainWindowController(...)
  → controller.start(testScriptPath)
      → TrackManager listener
      → Action bind
      → playback polling
      → optional TestRunner

MainWindow.dispose
  → controller.dispose()
      → Action unbind
      → playback/layout timers stop
      → state store dispose
      → analysis IPC dispose
      → track manager dispose
      → renderer dispose
```

原则：

- 所有全局注册都必须有对应释放点。
- `ActionRegistry` 绑定由 `MainWindowActionCoordinator` 持有生命周期。
- timer/ticker 由对应 coordinator 持有和释放。
- `VideoRendererController.dispose()` 是 fire-and-forget，但 controller public API 已有 disposed guard。

## 状态流

```text
Native / User event
  → Coordinator
  → MainWindowStateStore.setXxx()
  → notifyListeners()
  → MainWindow ListenableBuilder
  → MainWindowViewModel
  → MainWindowView
```

`MainWindowStateModel` 保存主窗口共享 UI 状态：

- texture / viewport 状态
- 播放状态、当前 PTS、duration、pending seek
- layout snapshot
- track sync offsets
- loop range 状态
- timeline hover / drag overlay 状态

规则：

- 写状态必须走 `MainWindowStateStore` 方法。
- 不在 `MainWindowView` 或子 widget 中写共享状态。
- 新字段加入 state model 后，必须同时补 copyWith 和 store 写入方法。
- 如果字段只属于某个 widget 的局部视觉状态，不要放进主窗口 store。

## Coordinator 所有权

### Playback

拥有：

- polling timer
- Dart fallback loop boundary timer
- loop range sync serial

负责：

- play / pause / seek / speed
- native playback state polling
- loop range enable/range sync
- startup loop range
- timeline hover state

新增播放控制功能优先放这里。

### Layout

拥有：

- viewport size
- layout dirty / resize dirty
- resize debounce timer
- layout ticker

负责：

- pan / zoom / split
- resize debounce
- `applyLayout` / `resize` / `getLayout` flush

新增 viewport 手势、布局模式、缩放行为优先放这里。

### Media

拥有或协调：

- open/add/remove track 流程
- track offset
- effective duration 计算
- last track removed reset callback

负责：

- 文件选择器和拖拽路径加载
- renderer create/add/remove track
- track offset 后刷新当前位置

新增轨道管理功能优先放这里。

### Analysis

拥有：

- analysis IPC server
- fileId → analysis hash cache
- snapshot serial

负责：

- 触发 analysis window
- 发布 track snapshot 到 analysis IPC

主播放器清理不主动重构 analysis 业务。

### Actions

拥有：

- 主窗口 Action 绑定表
- bind/unbind 生命周期

负责：

- 将 Action 绑定到 coordinator 方法
- 将测试脚本专用 Action 接到 `MainWindowTestHarness`

新增 Action 时先更新 `ACTION_MAINTENANCE.md`。

## View Model / Actions

`MainWindowView` 不直接读取 controller/coordinator，而是吃两个对象：

- `MainWindowViewModel`: 所有显示数据
- `MainWindowViewActions`: 所有 UI callback

新增 UI 控件时：

1. 显示数据放进 `MainWindowViewModel`。
2. 用户操作放进 `MainWindowViewActions`。
3. callback 由 `MainWindowController` 接到具体 coordinator。
4. `MainWindowView` 只做 widget composition。

如果 view model/actions 继续膨胀，可以按区域拆：

- `ToolbarViewModel/Actions`
- `ViewportViewModel/Actions`
- `TimelineViewModel/Actions`
- `TrackListViewModel/Actions`

## 跨 coordinator 流程

跨多个 coordinator 的流程放在 `MainWindowController`，不要让 coordinator 互相持有太多具体类型。

当前允许的跨域 callback：

- media 加载后调用 playback 的 startup loop range
- media 移除最后轨道时触发 controller reset
- track manager 改变时 controller 同步 layout order 并发布 analysis snapshot

新增跨域流程时，优先在 controller 中编排。

## 维护检查清单

新增主窗口功能前检查：

- 状态是否放在正确 owner？
- 是否需要 Action 或测试脚本指令？
- View 是否只吃 view model/actions？
- coordinator 是否有 dispose/timer 清理？
- 是否影响 `VideoRendererController` 调用时序？
- 是否补了对应 `ui_tests/*.csv` 或说明覆盖缺口？

修改后至少运行：

```bash
flutter analyze
python dev.py ui-test ui_tests/smoke_basic.csv
```

影响 timeline / seek / loop range：

```bash
python dev.py ui-test ui_tests/h265_timeline_click_visual_regression.csv
python dev.py ui-test ui_tests/h265_loop_range_enable_regression.csv
```

影响 viewport / layout：

```bash
python dev.py ui-test ui_tests/viewport_resize_center_regression.csv
python dev.py ui-test ui_tests/viewport_pan_layout_regression.csv
```
