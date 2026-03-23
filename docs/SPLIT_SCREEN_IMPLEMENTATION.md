# SPLIT_SCREEN 视图模式实现文档

## 功能概述

SPLIT_SCREEN（分屏对比）模式允许用户在同一位置重叠显示两个视频，通过可拖动的分割线来切换显示：
- 分割线**左侧**：显示 track 0 的内容
- 分割线**右侧**：显示 track 1 的内容

**UI 入口**：ToolBar 的 SegmentedWidget，包含"并排"和"分屏"两个选项

**Track 选择**：切换到分屏时只取 slot 最前的两个视频（0 和 1）渲染上屏，其他 track 不显示

## 模式说明

| 模式 | 枚举值 | 描述 |
|------|--------|------|
| SIDE_BY_SIDE | 0 | 所有视频等分显示 |
| SPLIT_SCREEN | 1 | 前两个视频重叠，分割线切换显示 |

## 视觉效果

```
SIDE_BY_SIDE 模式（并排）:
┌──────────┬──────────┐
│  Track0  │  Track1  │
│  Track2  │  Track3  │  <- 所有视频等分显示
└──────────┴──────────┘

SPLIT_SCREEN 模式（分屏对比）:
┌─────────────────────┐
│ Track0 │ Track1     │  <- 两个视频重叠在同一位置
│ (左侧) │ (右侧)     │     分割线左边显示 Track0
│        │            │     分割线右边显示 Track1
└────────┴────────────┘
   可拖动分割线位置
```

## 已修改的文件

### 1. ViewMode 枚举
**文件**: [player/ui/viewport/gl_widget.py](../player/ui/viewport/gl_widget.py)

```python
class ViewMode(IntEnum):
    """视图模式枚举"""
    SIDE_BY_SIDE = 0  # 并排模式 - 所有视频等分显示
    SPLIT_SCREEN = 1  # 分屏对比模式 - 重叠显示，分割线切换
```

### 2. Shader
**文件**: [player/shaders/multitrack.frag](../player/shaders/multitrack.frag)

```glsl
uniform int u_mode;  // 0 = SIDE_BY_SIDE, 1 = SPLIT_SCREEN

// main() 中：
if (u_mode == 1) {
    // SPLIT_SCREEN mode
    if (v_texCoord.x < u_split_pos) {
        track_idx = u_order[0];  // 左侧显示 track 0
    } else {
        track_idx = u_order[1];  // 右侧显示 track 1
    }
    local_uv = v_texCoord;  // 不分割 UV
    slot_aspect = u_canvas_aspect;  // 占满整个画布
} else {
    // SIDE_BY_SIDE mode
    ...
}
```

### 3. ViewportPanel
**文件**: [player/ui/viewport/panel.py](../player/ui/viewport/panel.py)

`_update_info_layout()` 在 SPLIT_SCREEN 模式下只显示前两个 MediaHeader。

### 4. 分割线拖动
**文件**: [player/ui/viewport/gl_widget.py](../player/ui/viewport/gl_widget.py)

`mousePressEvent`、`mouseMoveEvent`、`_is_near_split_line()` 支持 SPLIT_SCREEN 模式。

### 5. ToolBar
**文件**: [player/ui/toolbar.py](../player/ui/toolbar.py)

"分屏"按钮触发 `ViewMode.SPLIT_SCREEN`。

## 技术要点

1. **UV 坐标处理**：SPLIT_SCREEN 模式下，UV 坐标不进行区域映射，直接使用完整画布坐标
2. **Aspect Ratio**：每个 track 保持各自的 aspect ratio，`slot_aspect` 使用完整画布比例
3. **分割线拖动**：复用现有的拖动逻辑
4. **只显示前两个 track**：SPLIT_SCREEN 只处理 `u_order[0]` 和 `u_order[1]`
