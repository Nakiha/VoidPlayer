# Flutter 侧文档入口

本文档是 Flutter/Dart 层的入口索引。Native 渲染引擎文档见
[../native/docs/ARCHITECTURE.md](../native/docs/ARCHITECTURE.md)。

## 模块定位

Flutter 层负责 VoidPlayer 的跨平台 UI shell、主窗口/播放控制、Action 自动化入口、
platform service 注入、MethodChannel/EventChannel 调用编排，以及平台 texture 的 Flutter 侧展示。

核心边界：

- UI 只组合 widget 和 view model，不直接承载播放/布局业务。
- 主窗口业务由 `MainWindowController` 组合多个 coordinator。
- Native 播放、渲染与解码能力通过 `NativePlayerController` 暴露，Flutter 层不直接处理帧数据；平台 native target 与 runner compose 都在平台边界内。
- 跨平台主窗口在 `lib/main_window/`。Windows 专属能力集中在 `lib/windows/`，其中 analysis 窗口在 `lib/windows/analysis/`，跨窗口基础设施留在 `lib/windows/` 根目录。
- macOS runner/platform services 由 `macos/` 和平台能力开关承接；native playback 已可用，analysis UI/IPC 仍 capability-gated。

## 详细文档索引

| 文档 | 内容 |
|------|------|
| [Flutter 架构](docs/FLUTTER_ARCHITECTURE.md) | Flutter 层分层、目录、依赖规则、功能开发流程 |
| [主窗口架构](docs/MAIN_WINDOW_ARCHITECTURE.md) | `MainWindowController`、state store、coordinator、view model 的职责 |
| [Analysis 缓存与遮罩流程](docs/ANALYSIS_CACHE_OVERLAY.md) | VAC2 生成、VACHUNK 按需生成、seek 后 overlay refresh 的 Flutter 编排 |
| [Analysis Overlay Refresh 设计](docs/ANALYSIS_OVERLAY_REFRESH_DESIGN.md) | seek preview 事件驱动刷新、PTS/DTS 匹配、VACHUNK window 和 native chunk index 策略 |
| [Analysis 窗口架构](docs/ANALYSIS_WINDOW_ARCHITECTURE.md) | analysis app/page/workspace/chart/NALU/test runner 的职责边界 |
| [存储 Catalog](docs/STORAGE_CATALOG.md) | Flutter 侧 SQLite 索引、标注数据和缩略图落盘格式 |
| [App Feedback](docs/APP_FEEDBACK.md) | Flutter 侧轻量通知入口、适用边界和统一约束 |
| [Agent Protocol](docs/AGENT_PROTOCOL.md) | 常驻 agent 控制通道：连接文件、握手、方法清单、裁决导出文档 |
| [Action 设计](docs/ACTION_DESIGN.md) | 快捷键、UI 按钮、测试脚本共用的 Action 抽象 |
| [Action 维护](docs/ACTION_MAINTENANCE.md) | 新增/修改/移除 Action 与 Assert 的维护清单 |
| [AXTree 维护](docs/AXTREE_MAINTENANCE.md) | 主窗口 / analysis 窗口 Semantics、UIA、识图分割维护规则 |
| [UI 自动化测试](docs/UI_TESTING.md) | `ui_tests/` 目录分区、回归选择、补测试规则 |
| [macOS Runner](../macos/doc.md) | macOS Cocoa runner、native CVPixelBuffer target ring、Metal composition、release gate |
| [VoidPlayerCli](../installer/windows/docs/cli.md) | 发布包内只读 VAC2/VACHUNK cache 检查工具 |

## 常用开发命令

```bash
# Flutter 静态分析
flutter analyze

# 主窗口基础 UI 回归
python dev.py ui-test ui_tests/smoke/basic.csv

# timeline / seek / loop range 真实点击路径回归
python dev.py ui-test ui_tests/timeline/h265_timeline_click_visual_regression.csv
python dev.py ui-test ui_tests/loop/h265_loop_range_enable_regression.csv
```

更多测试选择见 [docs/UI_TESTING.md](docs/UI_TESTING.md)。

## 日志系统

默认所有模块日志级别为 `INFO`。正式安装版的运行时数据根目录是
`%APPDATA%\VoidPlayer`，若系统没有 `APPDATA` 则回退到
`%LOCALAPPDATA%\VoidPlayer`；带有 exe 旁 `cache/` 目录的便携式布局会把
运行时数据写在 exe 旁。

启动传参控制日志级别：

```bash
void_player.exe --log-level=flutter=DEBUG,native=TRACE,ffmpeg=INFO
```

- `flutter`: Flutter/Dart 层日志，写入运行时数据根目录下的 `logs/`
- `native`: C++ native 模块日志，写入运行时数据根目录下的 `logs/`
- `ffmpeg`: FFmpeg 库日志（预留，暂未实现）

## 启动参数

面向 release / GUI 用户的启动参数说明维护在
[../installer/windows/docs/gui.md](../installer/windows/docs/gui.md)，打包时会复制到
release staging 的 `docs/gui.md`。
