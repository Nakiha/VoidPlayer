# Flutter 侧文档入口

本文档是 Flutter/Dart 层的入口索引。Native 渲染引擎文档见
[../native/docs/ARCHITECTURE.md](../native/docs/ARCHITECTURE.md)。

## 模块定位

Flutter 层负责 VoidPlayer 的 Windows 桌面 UI、窗口协调、Action 自动化入口、
MethodChannel 调用编排，以及 DX11 texture 的 Flutter 侧展示。

核心边界：

- UI 只组合 widget 和 view model，不直接承载播放/布局业务。
- 主窗口业务由 `MainWindowController` 组合多个 coordinator。
- Native 渲染与解码能力通过 `VideoRendererController` 暴露，Flutter 层不直接处理帧数据。
- Windows 专属能力集中在 `lib/windows/`，其中主窗口在 `lib/windows/main/`，analysis 窗口在 `lib/windows/analysis/`，跨窗口基础设施留在 `lib/windows/` 根目录。

## 详细文档索引

| 文档 | 内容 |
|------|------|
| [Flutter 架构](docs/FLUTTER_ARCHITECTURE.md) | Flutter 层分层、目录、依赖规则、功能开发流程 |
| [主窗口架构](docs/MAIN_WINDOW_ARCHITECTURE.md) | `MainWindowController`、state store、coordinator、view model 的职责 |
| [Analysis 窗口架构](docs/ANALYSIS_WINDOW_ARCHITECTURE.md) | analysis app/page/workspace/chart/NALU/test runner 的职责边界 |
| [Action 设计](docs/ACTION_DESIGN.md) | 快捷键、UI 按钮、测试脚本共用的 Action 抽象 |
| [Action 维护](docs/ACTION_MAINTENANCE.md) | 新增/修改/移除 Action 与 Assert 的维护清单 |
| [UI 自动化测试](docs/UI_TESTING.md) | `ui_tests/` 目录分区、回归选择、补测试规则 |

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

默认所有模块日志级别为 `INFO`。日志文件落盘到 exe 旁的 `logs/` 目录。

启动传参控制日志级别：

```bash
void_player.exe --log-level=flutter=DEBUG,native=TRACE,ffmpeg=INFO
```

- `flutter`: Flutter/Dart 层日志，写入 `logs/void_player_YYYY-MM-DD.log`
- `native`: C++ native 模块日志，写入 `logs/native.log`
- `ffmpeg`: FFmpeg 库日志（预留，暂未实现）

## 启动参数

面向 release / GUI 用户的启动参数说明维护在
[../installer/windows/docs/gui.md](../installer/windows/docs/gui.md)，打包时会复制到
release staging 的 `docs/gui.md`。
