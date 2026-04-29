# Analysis 窗口架构

> Analysis 窗口用于 bitstream 可视化。它可以由主窗口 spawn，也可以作为 standalone analysis child process 运行。维护目标是让入口、页面状态、workspace、图表和测试 runner 各自收敛。

## 目录

Analysis 相关 Flutter 代码集中在 `lib/windows/analysis/`：

| 文件 | 职责 |
|------|------|
| `analysis_window.dart` | analysis app entry、主题、本地化、`AnalysisApp` / `AnalysisWorkspaceApp` |
| `analysis_window_page.dart` | 单个 analysis page 的薄入口、生命周期、test host 委托 |
| `analysis_page_controller.dart` | 单页数据加载、summary polling、派生状态、选择/zoom/filter 状态 |
| `analysis_page_state.dart` | 单页 view model、view actions、共享页面枚举 |
| `analysis_page_view.dart` | 单页页面布局编排，组装 chart、NALU browser/detail、split controls |
| `analysis_window_workspace.dart` | 多 track workspace、tab/split view、workspace header |
| `analysis_window_charts.dart` | reference pyramid、frame trend、chart scrollbar、chart painters |
| `analysis_window_nalu.dart` | NALU browser、filter、detail panel |
| `analysis_window_controls.dart` | order/tab controls、analysis view icon、resizable dividers |
| `analysis_window_test_runner.dart` | analysis 子窗体 CSV 指令解析和断言执行 |
| `analysis_ipc.dart` | 主窗口和 analysis workspace process 之间的 track snapshot IPC |
| `analysis_test_host.dart` | test runner 访问页面状态的窄接口 |
| `analysis_split_layout_controller.dart` | workspace split view 共享布局比例 |
| `analysis_window_style.dart` | analysis header/control 尺寸常量 |
| `analysis_frame_utils.dart` | frame/slice 显示 helper |

## 边界

- `analysis_window.dart` 保持薄入口，不放页面状态和交互逻辑。
- `analysis_window_page.dart` 保持薄壳，不放页面级数据状态；状态和交互逻辑归 `analysis_page_controller.dart`。
- 图表绘制和 hit-test 逻辑归 `analysis_window_charts.dart`。
- NALU 列表/详情归 `analysis_window_nalu.dart`。
- 主窗口触发 analysis 的流程在 `lib/windows/main/main_window_analysis.dart`；跨进程生命周期在 `lib/windows/window_manager.dart`。
- analysis 子窗体脚本只放 analysis 专属指令，不复用主窗口 `PlayerAction`。
- Analysis 文件之间使用普通 `import`，不要重新引入 `part` / `part of` 来共享私有状态。跨文件需要访问页面状态时，优先补窄接口或 view model。
- `analysis_window_test_runner.dart` 只能通过 `AnalysisTestHost` 访问 page state，避免测试 DSL 再次和 `_AnalysisPageState` 私有字段耦合。

## 测试选择

Analysis UI 改动不应只跑 `ui_tests/smoke/basic.csv`。

从 `ui_tests/analysis/` 选脚本：

- 修改主窗口触发、spawn、workspace track 更新：跑 `spawn_*.csv` 或 `ipc_*.csv`。
- 修改 analysis 子窗体页面、chart、NALU detail、order/tab 行为：通过主窗口 `spawn_*.csv` 带 `child_*.csv` 验证。
- 新增 analysis 子窗体交互时，优先扩展 `analysis_window_test_runner.dart` 和 `ui_tests/analysis/child_*.csv`。

`child_*.csv` 是由父脚本通过 `SET_ANALYSIS_TEST_SCRIPT` 传给 analysis 子窗体的辅助脚本，不是普通主窗口脚本。
