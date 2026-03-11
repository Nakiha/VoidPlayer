# VoidPlayer 开发规范

## UI 间距规范

### 边距
- **上下边距**: 固定 4px
- **左右边距**: 页面级别固定 8px

### 组件间距
- **组件间 space**: 固定 4px

### 示例
```python
# 布局设置
layout.setContentsMargins(8, 4, 8, 4)  # 左8, 上4, 右8, 下4
layout.setSpacing(4)  # 组件间距 4px
```

## 文件结构
- `player/` - 播放器核心模块
  - `main_window.py` - 主窗口
  - `toolbar.py` - 顶部工具栏
  - `media_info_bar.py` - 媒体信息条
  - `controls_bar.py` - 播放控制条
  - `timeline_area.py` - 时间轴区域
  - `viewport_panel.py` - 视频预览区域
