# QFont::setPointSize 警告修复

## 问题描述

当鼠标悬停在 `TransparentToolButton` 图标按钮上再移出时，控制台会打印警告：

```
QFont::setPointSize: Point size <= 0 (-1), must be greater than 0
```

## 原因分析

这是 `qfluentwidgets` 库中 `TransparentToolButton` 的内部问题：

1. 按钮在创建时，字体大小可能未正确初始化（`pointSize() = -1`）
2. 当鼠标悬停状态变化时，`qfluentwidgets` 内部会尝试设置字体
3. 由于字体大小为 `-1`，Qt 会打印警告

## 解决方案

### 1. 创建辅助函数 `create_tool_button()`

在 [player/widgets/buttons.py](player/widgets/buttons.py) 中创建辅助函数：

```python
from PySide6.QtGui import QFont
from qfluentwidgets import TransparentToolButton, FluentIcon


def create_tool_button(icon: FluentIcon, parent=None, size: int = 28) -> TransparentToolButton:
    """
    创建一个修复了字体问题的 TransparentToolButton

    修复: qfluentwidgets 的 TransparentToolButton 在悬停时会触发
    QFont::setPointSize: Point size <= 0 (-1) 警告
    """
    btn = TransparentToolButton(icon, parent)
    btn.setFixedSize(size, size)

    # 修复字体问题: 设置一个有效的字体确保 pointSize > 0
    font = btn.font()
    if font.pointSize() <= 0:
        font.setPointSize(9)  # 设置一个默认的有效字体大小
        btn.setFont(font)

    return btn
```

### 2. 替换所有直接使用 `TransparentToolButton` 的地方

**修复前：**
```python
self.btn = TransparentToolButton(FluentIcon.SETTING, self)
self.btn.setFixedSize(28, 28)
```

**修复后：**
```python
from .widgets import create_tool_button

self.btn = create_tool_button(FluentIcon.SETTING, self, 28)
```

## 修改的文件

| 文件 | 修改内容 |
|------|----------|
| [player/widgets/buttons.py](player/widgets/buttons.py) | 新增 `create_tool_button()` 函数 |
| [player/controls_bar.py](player/controls_bar.py) | 所有按钮改用 `create_tool_button()` |
| [player/track_row.py](player/track_row.py) | 所有按钮改用 `create_tool_button()` |
| [player/media_info_bar.py](player/media_info_bar.py) | 所有按钮改用 `create_tool_button()` |
| [player/toolbar.py](player/toolbar.py) | 所有按钮改用 `create_tool_button()` |

## 验证

运行程序后，鼠标悬停在任何图标按钮上再移出，不再打印字体警告。
