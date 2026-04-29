# Analysis 窗口架构

> Analysis 窗口用于 bitstream 可视化。它可以由主窗口 spawn，也可以作为 standalone analysis child process 运行。维护目标是让入口、页面状态、workspace、图表和测试 runner 各自收敛。

## 目录

Analysis 相关 Flutter 代码集中在 `lib/windows/analysis/`：

```text
analysis/
├── analysis_window.dart           # app entry、theme/localization、AnalysisApp / AnalysisWorkspaceApp
├── page/                          # 单个 analysis 页面状态和布局
├── workspace/                     # 多 track workspace、tabs/split 模式
├── charts/                        # reference pyramid / frame trend 图表
├── widgets/                       # NALU、controls、style、split layout 等页面 widget
├── ipc/                           # main process 与 analysis process 的 track snapshot IPC
└── testing/                       # analysis 子窗体测试 host 和 CSV runner
```

| 目录 | 职责 |
|------|------|
| `page/` | 单页生命周期、数据加载、派生状态、view model/actions、页面布局编排 |
| `workspace/` | 多 track workspace 薄入口、IPC snapshot 合并、tabs/split header/pane |
| `charts/` | chart 兼容导出入口、共享坐标轴/scrollbar、各图表 painter 与交互 |
| `widgets/` | NALU browser/detail、order/tab controls、analysis style、split layout controller、frame helper |
| `ipc/` | IPC model/server/client；main 侧只依赖 server/model，analysis 子进程只依赖 client/model |
| `testing/` | test runner 访问页面状态的窄接口和 analysis 子窗体 CSV 指令 |

## 边界

- `analysis_window.dart` 保持薄入口，不放页面状态和交互逻辑。
- `page/analysis_page.dart` 保持薄壳，不放页面级数据状态；状态和交互逻辑归 `page/analysis_page_controller.dart`。
- 图表绘制和 hit-test 逻辑按图表类型拆分；共享坐标轴/scrollbar 逻辑归 `charts/analysis_chart_common.dart`。
- NALU 列表/详情归 `widgets/analysis_nalu.dart`。
- 主窗口触发 analysis 的流程在 `lib/windows/main/main_window_analysis.dart`；跨进程生命周期在 `lib/windows/window_manager.dart`。
- 主窗口只依赖 `ipc/analysis_ipc_server.dart` / `ipc/analysis_ipc_models.dart`；analysis 子进程只依赖 `ipc/analysis_ipc_client.dart` / `ipc/analysis_ipc_models.dart`。
- analysis 子窗体脚本只放 analysis 专属指令，不复用主窗口 `PlayerAction`。
- Analysis 文件之间使用普通 `import`，不要重新引入 `part` / `part of` 来共享私有状态。跨文件需要访问页面状态时，优先补窄接口或 view model。
- `testing/analysis_test_runner.dart` 只能通过 `AnalysisTestHost` 访问 page state，避免测试 DSL 再次和 `_AnalysisPageState` 私有字段耦合。

## 测试选择

Analysis UI 改动不应只跑 `ui_tests/smoke/basic.csv`。

从 `ui_tests/analysis/` 选脚本：

- 修改主窗口触发、spawn、workspace track 更新：跑 `spawn_*.csv` 或 `ipc_*.csv`。
- 修改 analysis 子窗体页面、chart、NALU detail、order/tab 行为：通过主窗口 `spawn_*.csv` 带 `child_*.csv` 验证。
- 新增 analysis 子窗体交互时，优先扩展 `testing/analysis_test_runner.dart` 和 `ui_tests/analysis/child_*.csv`。

`child_*.csv` 是由父脚本通过 `SET_ANALYSIS_TEST_SCRIPT` 传给 analysis 子窗体的辅助脚本，不是普通主窗口脚本。
