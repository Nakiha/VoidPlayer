# Flutter 层架构概览

> 本文档描述 Flutter/Dart 层的长期维护边界。新增复杂 UI 功能前，先确认是否符合本文的分层和测试约定。

## 模块定位

Flutter 层是 Windows 桌面播放器 UI 和 native 渲染引擎之间的编排层，负责：

- 主窗口 UI、timeline、loop range、轨道列表和 viewport 交互
- 多窗口协调：设置、统计、内存、analysis 子窗口
- Action 系统：快捷键、按钮、测试脚本共用同一套操作入口
- UI 自动化脚本和截图/hash 回归闭环
- 通过 `VideoRendererController` 调用 native MethodChannel

Flutter 层不负责：

- 解码、帧同步、D3D11 texture 创建和渲染
- FFmpeg/D3D11 资源生命周期细节
- analysis 数据计算本身

## 目录结构

```text
lib/
├── actions/                       # Action / Assert / TestRunner
├── docs/                          # Flutter 层设计和维护文档
├── l10n/                          # 本地化资源和 action label 映射
├── widgets/                       # 可复用 UI 控件
├── windows/                       # Windows 专属窗口、平台交互和主窗口编排
│   ├── main_window.dart           # 薄 StatefulWidget shell
│   ├── main_window_controller.dart# 主窗口 facade，组合 coordinator
│   ├── main_window_state.dart     # immutable state model + listenable store
│   ├── main_window_view.dart      # 纯 view，吃 view model + actions
│   ├── main_window_actions.dart   # Action 绑定生命周期
│   ├── main_window_media.dart     # 打开/添加/移除媒体和轨道刷新
│   ├── main_window_layout.dart    # viewport pan/zoom/split/resize flush
│   ├── main_window_playback.dart  # play/pause/seek/poll/loop range
│   ├── main_window_analysis.dart  # analysis IPC snapshot 和窗口触发
│   └── window_manager.dart        # desktop_multi_window / Win32 窗口协调
├── video_renderer_controller.dart # native MethodChannel API wrapper
└── main.dart                      # app bootstrap 入口
```

## 分层

```text
Widgets / Views
  ↓ 只读 view model + callbacks
MainWindowController
  ↓ 组合 state store / coordinators / services
Coordinators
  ↓ 调用 VideoRendererController、TrackManager、WindowManager
Platform / Native bridge
  ↓ MethodChannel / Win32 FFI / desktop_multi_window
Native renderer
```

### View 层

View 层只做布局和控件组合。

规则：

- 不直接调用 `VideoRendererController`。
- 不保存播放、seek、layout 等业务状态。
- 不读取 native 状态。
- 通过 `MainWindowViewModel` 读数据，通过 `MainWindowViewActions` 发事件。

### Controller / Coordinator 层

`MainWindowController` 是主窗口 facade，负责装配和跨域协调。具体业务由 coordinator 持有：

- playback: 播放、暂停、seek、polling、loop range
- layout: viewport resize、pan、zoom、split、layout flush
- media: open/add/remove track、offset、effective duration
- analysis: analysis IPC snapshot 和窗口触发
- actions: ActionRegistry 绑定和解绑

规则：

- 新功能优先放进拥有对应状态的 coordinator。
- 跨多个 coordinator 的流程放进 `MainWindowController`。
- 不把业务逻辑塞回 `main_window.dart`。
- 不让 widget 直接依赖 coordinator 的内部状态。

### State 层

`MainWindowStateModel` 是 immutable state snapshot，`MainWindowStateStore` 是 `ChangeNotifier`。

规则：

- 状态只能通过 store 方法写入。
- coordinator 通过 getter closure 读取当前 state，通过 store callback 写状态。
- UI 通过 `ListenableBuilder` 响应 store 变化。
- 需要新增 UI 状态时，先判断状态归属：主窗口 UI 状态放 store，临时控件 hover/drag 可留在具体 widget，native 资源状态留 native/controller。

### Platform / Native bridge

`VideoRendererController` 是 Dart 层唯一的 renderer MethodChannel wrapper。Windows 专属能力放在 `lib/windows/`。

规则：

- 文件选择器、窗口句柄、Win32 FFI 不要进入通用 widgets。
- 新 native 方法要在 `VideoRendererController` 做 payload 校验和 disposed guard。
- 影响上屏/seek/resize 的改动必须配套 UI 自动化脚本。

## 开发流程

新增复杂 UI 功能时按这个顺序考虑：

1. 定义状态归属：state store、coordinator 内部字段、widget 局部状态还是 native 状态。
2. 定义交互入口：是否需要 Action、快捷键、测试脚本指令。
3. 扩展 view model/actions：让 view 只吃数据和 callback。
4. 实现 coordinator 逻辑：避免 view 直接操作 renderer。
5. 补 UI 自动化：优先真实点击路径，不够时补 Action/Assert。
6. 运行对应 GUI 回归，最终说明缺口。

## 禁止回退的旧模式

以下模式是历史债务来源，后续不要重新引入：

- `part of 'main_window.dart'` 共享 `_MainWindowState` 私有字段
- 在 widget build 附近写播放、seek、resize、track 业务流程
- 控件直接调用 MethodChannel
- Action 只 bind 不 dispose/unbind
- 大功能只靠人工试点，不补脚本或 Assert
- 因为某个参数方便，就继续扩大 `MainWindowView` 构造函数而不分组

## Analysis UI

`analysis_window.dart` 当前体量较大，但它不是主播放器 UI 这轮清理的重点。除非主窗口架构或 Action/TestRunner 变更影响 analysis，否则不要在播放器主线清理中主动重构 analysis。analysis 后续需要堆功能时，应单独开一轮文档和拆分。
