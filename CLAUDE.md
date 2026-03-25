# 规范

## 依赖

- UI 控件必须优先使用 PySide6-Fluent-Widgets
- PySide6-Fluent-Widgets-Nuitka 为本项目的特殊分发版

## Native 模块

修改 `native/` 代码后运行：`python build.py native`

## 验证

修改后运行 mock 测试检查错误：`python run_player.py --mock tests/mock/basic.vpmock`

## 控制日志级别
默认均为 INFO
`python run_player.py -l default=INFO,ffmpeg=INFO,opengl=INFO`
- `default`: Python 和 native 模块
- `ffmpeg`: FFmpeg 库
- `opengl`: 仅 DEBUG 时有日志 gl_debug_callback

## UI

- 控件间距固定 4px，避免父子控件重复加边距
- 固有色必须在 `player/theme_utils.py` 管理, 通过 `get_color_hex(ColorKey.${枚举值})` 获取
- 自定义控件在 `player/ui/widgets/`

## 交互功能 (action)

新增交互功能检查清单：

1. 交互注册: `player/core/actions/registry.py`
2. 快捷键: `player/core/shortcuts.py`
3. Mock 测试: `tests/mock/`

详见 docs/SHORTCUTS.md 和 docs/MOCK_TESTING.md

## 任务完成通知

用户要求发送通知时, 在任务完成后
1. 写入 `notify_content.json` 格式 `{"title": "[简短标题]", "message": "[关键结论/状态]"}`
2. 执行 `python notify.py`