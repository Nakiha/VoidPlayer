# Flutter 侧文档

## 日志系统

默认所有模块日志级别为 `INFO`。日志文件落盘到 exe 旁的 `logs/` 目录。

### Flutter 侧

启动传参控制日志级别：

```
void_player.exe --log-level=flutter=DEBUG,native=TRACE,ffmpeg=INFO
```

- `flutter`: Flutter (Dart) 层日志，写入 `logs/void_player_YYYY-MM-DD.log`
- `native`: C++ native 模块日志，写入 `logs/native.log`
- `ffmpeg`: FFmpeg 库日志（预留,暂未实现）

## 启动参数

面向 release / GUI 用户的启动参数说明维护在 [../release/gui.md](../release/gui.md)。

## 交互 (Action)

统一管理快捷键、UI 按钮、测试脚本的操作抽象层

- **[docs/ACTION_DESIGN.md](docs/ACTION_DESIGN.md)** — Action 系统设计文档
- **[docs/ACTION_MAINTENANCE.md](docs/ACTION_MAINTENANCE.md)** — Action 系统维护指南

### 新增交互功能检查清单

1. **定义 Action**: 在 `lib/actions/player_action.dart` 添加 sealed subclass
2. **注册快捷键**: Action 构造函数传入 `LogicalKeyboardKey`（无快捷键则省略）
3. **绑定回调**: 控件 `initState` 中 `actionRegistry.bind(action, callback)`，`dispose` 中 `unbind(name)`
4. **UI 触发点**: 按钮等通过 `actionRegistry.execute('ACTION_NAME')` 触发
5. **测试脚本**: 如需 mock 测试，在 `lib/actions/test_runner.dart` 的 `_parseInstruction` 中添加指令解析
6. **闭环验证**: 优先补/改 `ui_tests/*.csv`，然后执行 `python dev.py ui-test ui_tests/smoke_basic.csv` 或对应脚本

### UI 自动化回归

当前项目已经支持通过启动参数 `--test-script <csv>` 在主窗口启动后自动执行脚本，`dev.py` 已封装为：

```bash
python dev.py ui-test ui_tests/smoke_basic.csv
```

建议约定：

- 通用冒烟回归放在 `ui_tests/smoke_basic.csv`
- 某个 bug 的复现/防回归单独放一条 `ui_tests/*.csv`
- 修改 Action / 播放控制 / 主窗口交互后，至少跑一条相关脚本
- 如果脚本覆盖不了，优先补 Action / Assert / 启动参数，而不是退回纯人工验证

## Windows 平台层

Windows 专属的 Dart 实现集中在 `lib/windows/`：

- `win32ffi.dart`: raw Win32 FFI，负责窗口查找、移动、关闭、鼠标按键状态等。
- `window_manager.dart`: secondary window / analysis process 生命周期协调。
- `native_file_picker.dart`: Windows IFileDialog 的 MethodChannel wrapper。通用 `VideoRendererController` 不再包含文件选择器逻辑，只保留 renderer 控制 API。

### 主窗口拆分

`main_window.dart` 保留 `State` 字段、生命周期和 widget composition；成片行为拆到同 library 的 part 文件：

- `main_window_actions.dart`: ActionRegistry 绑定表。
- `main_window_media.dart`: 打开文件、拖拽、ADD_MEDIA 共享的 media loading 流程。
- `main_window_layout.dart`: viewport pan/zoom/split、resize、layout flush。
- `main_window_playback.dart`: play/pause/seek、polling、loop range、timeline hover/click。
- `main_window_tracks.dart`: track remove/reorder/offset 和 effective duration。
- `main_window_analysis.dart`: analysis hash、IPC snapshot、analysis window trigger。

这些 part 文件暂时共享 `_MainWindowState` 的私有字段，目的是先划清工程边界而不引入状态管理框架；后续如果某块继续膨胀，再把对应 part 提升成独立 controller/session。

跨平台整理时，优先把新平台实现放到明确的平台层，再由主窗口组合调用；不要把 Win32 / MethodChannel 细节塞回通用 controller。
