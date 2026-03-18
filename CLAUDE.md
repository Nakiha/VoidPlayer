# 规范

## 依赖

- 新增python依赖更新 `pyproject.toml`
- UI控件必须优先使用 PySide6-Fluent-Widgets，保持风格一致

## Native 模块

修改 `native/` 目录下的代码后，运行构建脚本：

```bash
python build_native.py
# 产物会自动安装到 Python site-packages 目录
```

## 验证
修改后5秒超时运行 `python run_player.py -i resources/video/NovosobornayaSquare_1920x1080.mp4 -i resources/video/TheaterSquare_1920x1080.mp4 --auto-play` 以检查错误

## UI

- 控件间距固定 4px，注意不要在父子控件中重复加边距
- 所有固有色必须通过 `player/theme_utils.py` 统一管理，获取使用 `get_color_hex(ColorKey.${枚举值})`

## 通用控件 (player/ui/widgets/)

需要类似功能时优先复用，不要新建：

| 控件 | 用途 |
|------|------|
| TimeLabel | 播放时间显示 (当前/总时长) |
| OffsetLabel | 偏移时间显示 (带正负号) |
| ElideLabel | 文本过长时自动省略，悬停显示完整 tooltip |
| DraggableElideLabel | 可拖拽的省略文本标签，用于轨道重排序 |
| ElideComboBox | 下拉框显示 basename，悬停显示完整路径 tooltip |
| HighlightSplitter | 悬浮高亮分割器 |
| ResizableContainer | 可拖动调整大小的容器 |
| create_tool_button | 创建工具按钮 (修复字体警告) |
