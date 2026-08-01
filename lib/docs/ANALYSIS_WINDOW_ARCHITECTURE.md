# Analysis 内嵌 deck 架构

> Analysis 用于 bitstream 可视化，作为主窗口底部 deck 的常驻 tab 运行。帧与
> NALU 数据通过进程内 FFI 直接读取 VAC2 cache，不存在 analysis 子进程或 IPC。

## 目录

Analysis UI 集中在 `lib/analysis/ui/`：

```text
ui/
├── page/                          # 单个 analysis 页面状态和布局
├── workspace/                     # 多 track workspace、tabs/split 模式
├── charts/                        # reference pyramid / frame trend 图表
├── widgets/                       # NALU、controls、style、split layout
└── testing/                       # AnalysisTestHost 窄接口
```

`MainWindowDeck` 使用 `IndexedStack` 保活 timeline 与 analysis 两个 tab。
`MainWindowAnalysisCoordinator.entries` 是 workspace 的进程内
`ValueListenable<List<AnalysisWorkspaceEntry>>` 数据源；条目按 track 的稳定
`fileId` 对齐，并携带可空 hash 与 `AnalysisTrackGenerationStatus`。hash 未就绪时
workspace 显示生成状态，cache 就绪后才创建 `AnalysisPage`。

## 边界

- `page/analysis_page.dart` 保持薄壳；数据状态和交互逻辑归
  `page/analysis_page_controller.dart`。
- 图表绘制和 hit-test 按图表类型拆分；共享坐标轴/scrollbar 逻辑归
  `charts/analysis_chart_common.dart`。
- NALU 列表/详情归 `widgets/analysis_nalu.dart`。
- 主窗口进入 analysis、生成 cache、同步 track entries 与 overlay 编排归
  `lib/main_window/main_window_analysis.dart`。
- workspace 直接继承主窗口 `Theme`，不维护独立 accent 参数。
- Analysis 文件使用普通 `import`，不要重新引入 `part` / `part of`。
- `testing/analysis_test_host.dart` 定义页面测试窄接口与当前页面 registry；
  `analysis_test_executor.dart` 执行主窗口 CSV 分发来的 analysis 命令。

## 测试选择

页面状态、workspace entry 合并和 deck 切换优先使用 Dart/widget 测试。只有真实
native cache/FFI、窗口尺寸或跨 Flutter/native 时序无法下沉时，才更新
`ui_tests/analysis/` 中的单进程脚本。
