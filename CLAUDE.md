# 规范

## 依赖

- 新增python依赖更新 `pyproject.toml`
- UI控件必须优先使用 PySide6-Fluent-Widgets，保持风格一致

## 验证

修改后运行 `timeout 1 python run_player.py 2>&1` 检查基础错误

## UI

- 控件间距固定 4px，注意不要在父子控件中重复加边距
- 所有固有色必须通过 `player/theme_utils.py` 统一管理，获取使用 `get_color_hex(ColorKey.${枚举值})`

## 通用控件 (player/widgets/)

需要类似功能时优先复用，不要新建：

| 控件 | 用途 |
|------|------|
| TimeLabel | 播放时间显示 (当前/总时长) |
| OffsetLabel | 偏移时间显示 (带正负号) |
| HighlightSplitter | 悬浮高亮分割器 |
| CheckableToolButton | 可切换状态按钮 (checked 时主题色高亮) |
| ResizableContainer | 可拖动调整大小的容器 |
| create_tool_button | 创建工具按钮 |
