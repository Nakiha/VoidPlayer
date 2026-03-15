# VoidPlayer

本项目是一个可以并排/分屏对比多个视频的播放器

# 规范

## 依赖管理

新增 Python 依赖时，需要更新 `pyproject.toml`

### 当前依赖

PySide6: GUI框架
PySide6-Fluent-Widgets: 控件库，为保持UI风格一致，必须优先使用该库中的控件

## 代码修改验证

修改后运行一次 `timeout 1 python run_player.py 2>&1` 以检查是否有基础错误

## UI 间距

控件间距固定 4px
注意不要为容器和控件添加重复的边距，这会导致视觉上控件间距超过4px

## 固有色

所有固有色必须通过 `player/theme_utils.py` 统一管理，获取使用 `get_color_hex(ColorKey.${枚举值})`

## 通用控件 (player/widgets/)

| 控件 | 用途 |
|------|------|
| TimeLabel | 播放时间显示 (当前/总时长) |
| OffsetLabel | 偏移时间显示 (带正负号) |
| HighlightSplitter | 悬浮高亮分割器 |
| CheckableToolButton | 可切换状态按钮 (checked 时主题色高亮) |
| ResizableContainer | 可拖动调整大小的容器 |
| create_tool_button | 创建工具按钮 |

## 文件结构

```
player/
├── main_window.py      # 主窗口
├── toolbar.py          # 顶部工具栏
├── controls_bar.py     # 播放控制条
├── timeline_area.py    # 时间轴区域
├── viewport/           # 视频预览模块
└── widgets/            # 通用控件
```
