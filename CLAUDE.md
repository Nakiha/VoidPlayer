# 规范

## 依赖

- 新增 Python 依赖更新 `pyproject.toml`
- UI 控件必须优先使用 PySide6-Fluent-Widgets

## Native 模块

修改 `native/` 目录后运行：`python build_native.py`

## 验证

修改后运行 mock 测试检查错误：`python run_player.py --mock tests/mock/basic.vpmock`

## 日志

`-l/--log-level` 控制日志级别，如：`python run_player.py -l default=DEBUG,ffmpeg=DEBUG`

- `default`: Python 和 native 模块日志
- `ffmpeg`: FFmpeg 库日志

## UI

- 控件间距固定 4px，避免父子控件重复加边距
- 固有色必须通过 `player/theme_utils.py` 统一管理, 通过 `get_color_hex(ColorKey.${枚举值})` 获取
- 自定义控件位于 `player/ui/widgets/`，优先复用

## 交互功能

新增交互功能检查清单：

1. **动作注册**: `player/core/action_registry.py`
2. **快捷键**: `player/core/shortcuts.py`
3. **Mock 测试**: `tests/mock/`

详见 [docs/SHORTCUTS.md](docs/SHORTCUTS.md) 和 [docs/MOCK_TESTING.md](docs/MOCK_TESTING.md)

## 回复完成通知

**重要, 每次响应用户请求后，必须按以下步骤发送通知:**

1. 使用 Write 工具写入 `notify_content.json`:
   `{"title": "[简短标题]", "message": "[关键结论/状态]"}`
2. 执行 `python notify.py`