# Analysis 窗口架构

> Analysis 窗口用于 bitstream 可视化。它可以由主窗口 spawn，也可以作为 standalone analysis child process 运行。维护目标是让入口、页面状态、workspace、图表和测试 runner 各自收敛。

## 目录

Analysis 相关 Flutter 代码集中在 `lib/windows/analysis/`：

| 文件 | 职责 |
|------|------|
| `analysis_window.dart` | analysis app entry、主题、本地化、`AnalysisApp` / `AnalysisWorkspaceApp` |
| `analysis_window_page.dart` | 单个 analysis page 的数据加载、派生状态、选择状态、页面布局编排 |
| `analysis_window_workspace.dart` | 多 track workspace、tab/split view、workspace header |
| `analysis_window_charts.dart` | reference pyramid、frame trend、chart scrollbar、chart painters |
| `analysis_window_nalu.dart` | NALU browser、filter、detail panel |
| `analysis_window_controls.dart` | order/tab controls、analysis view icon、resizable dividers |
| `analysis_window_test_runner.dart` | analysis 子窗体 CSV 指令解析和断言执行 |
| `analysis_ipc.dart` | 主窗口和 analysis workspace process 之间的 track snapshot IPC |

## 边界

- `analysis_window.dart` 保持薄入口，不放页面状态和交互逻辑。
- `analysis_window_page.dart` 可以协调页面级状态，但不要继续塞 chart painter、NALU list 或 workspace UI。
- 图表绘制和 hit-test 逻辑归 `analysis_window_charts.dart`。
- NALU 列表/详情归 `analysis_window_nalu.dart`。
- 主窗口触发 analysis 的流程在 `lib/windows/main/main_window_analysis.dart`；跨进程生命周期在 `lib/windows/window_manager.dart`。
- analysis 子窗体脚本只放 analysis 专属指令，不复用主窗口 `PlayerAction`。

## 测试选择

Analysis UI 改动不应只跑 `ui_tests/smoke/basic.csv`。

从 `ui_tests/analysis/` 选脚本：

- 修改主窗口触发、spawn、workspace track 更新：跑 `spawn_*.csv` 或 `ipc_*.csv`。
- 修改 analysis 子窗体页面、chart、NALU detail、order/tab 行为：通过主窗口 `spawn_*.csv` 带 `child_*.csv` 验证。
- 新增 analysis 子窗体交互时，优先扩展 `analysis_window_test_runner.dart` 和 `ui_tests/analysis/child_*.csv`。

`child_*.csv` 是由父脚本通过 `SET_ANALYSIS_TEST_SCRIPT` 传给 analysis 子窗体的辅助脚本，不是普通主窗口脚本。
