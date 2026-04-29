# Flutter 层架构概览

> 本文档描述 Flutter/Dart 层的长期维护边界。新增复杂 UI 功能前，先确认是否符合本文的分层和测试约定。

## 模块定位

Flutter 层是 Windows 桌面播放器 UI 和 native 渲染引擎之间的编排层，负责：

- 主窗口 UI、timeline、loop range、轨道列表和 viewport 交互
- 窗口协调：主窗口内设置/统计/内存浮层，以及独立进程 analysis 窗口
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
│   ├── main/                      # 主播放器窗口 shell/controller/coordinators/view
│   │   ├── main_window.dart       # 薄 StatefulWidget shell
│   │   ├── main_window_controller.dart
│   │   ├── main_window_state.dart
│   │   ├── main_window_view.dart
│   │   ├── main_window_actions.dart
│   │   ├── main_window_media.dart
│   │   ├── main_window_layout.dart
│   │   ├── main_window_playback.dart
│   │   └── main_window_analysis.dart
│   ├── analysis/                  # analysis 窗口 app/page/widgets/charts/IPC
│   │   ├── analysis_window.dart   # analysis app entry
│   │   ├── analysis_window_page.dart
│   │   ├── analysis_window_workspace.dart
│   │   ├── analysis_workspace_models.dart
│   │   ├── analysis_workspace_tabs.dart
│   │   ├── analysis_workspace_split.dart
│   │   ├── analysis_workspace_mode_toggle.dart
│   │   ├── analysis_window_charts.dart
│   │   ├── analysis_window_nalu.dart
│   │   ├── analysis_window_controls.dart
│   │   ├── analysis_window_test_runner.dart
│   │   ├── analysis_test_host.dart
│   │   ├── analysis_split_layout_controller.dart
│   │   ├── analysis_ipc.dart
│   │   ├── analysis_ipc_models.dart
│   │   ├── analysis_ipc_server.dart
│   │   └── analysis_ipc_client.dart
│   ├── app_bootstrap.dart         # 主窗口和 standalone analysis 启动分发
│   └── window_manager.dart        # analysis 进程 / Win32 窗口协调
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
  ↓ MethodChannel / Win32 FFI / analysis IPC
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

Analysis 窗口代码集中在 `lib/windows/analysis/`。`analysis_window.dart` 只保留 app entry 和主题壳；页面状态、workspace、chart painter、NALU browser/detail、测试脚本 runner 分文件维护，并使用普通 `import` 连接，禁止重新引入 `part` / `part of`。修改 analysis UI 时优先参考 [ANALYSIS_WINDOW_ARCHITECTURE.md](ANALYSIS_WINDOW_ARCHITECTURE.md)，并从 `ui_tests/analysis/` 选择主窗口 spawn 或子窗体脚本做闭环验证。
