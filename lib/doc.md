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
