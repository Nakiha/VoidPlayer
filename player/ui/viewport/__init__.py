"""
Viewport 模块 - 视频预览区域相关组件
"""
from .panel import ViewportPanel, ViewMode
from .gl_widget import MultiTrackGLWindow, MultiTrackGLWidget  # 兼容别名
from .placeholder import VideoPlaceholder
from .header import MediaHeader

__all__ = [
    'ViewportPanel',
    'ViewMode',
    'MultiTrackGLWindow',
    'MultiTrackGLWidget',  # 向后兼容
    'VideoPlaceholder',
    'MediaHeader',
]
