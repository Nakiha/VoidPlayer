# VoidPlayer 开发规范

## 依赖管理

新增 Python 依赖时，需要更新 `pyproject.toml`

## UI 间距规范

- **上下边距**: 固定 4px
- **左右边距**: 页面级别固定 8px
- **组件间 space**: 固定 4px

## 固有色规范

所有固有色必须通过 `player/theme_utils.py` 统一管理，获取使用 `get_color_hex(ColorKey.${枚举值})`

## 通用控件 (player/widgets/)

| 控件 | 用途 |
|------|------|
| `TimeLabel` | 播放时间显示 (当前/总时长) |
| `OffsetLabel` | 偏移时间显示 (带正负号) |
| `ResizableContainer` | 可拖动边缘调整大小的容器 |
| `create_tool_button(icon, parent, size)` | 创建工具按钮 |

## 文件结构
- `player/` - 播放器核心模块
  - `main_window.py` - 主窗口
  - `toolbar.py` - 顶部工具栏
  - `media_info_bar.py` - 媒体信息条
  - `controls_bar.py` - 播放控制条
  - `timeline_area.py` - 时间轴区域
  - `viewport_panel.py` - 视频预览区域
  - `widgets/` - 通用控件
