"""
VoidPlayer - Multi-video playback module
"""

# Logging
from .logging_config import setup_logging, get_logger

# UI 组件 (无 native 依赖)
from .view_mode import ViewMode
from .main_window import MainWindow
from .toolbar import ToolBar
from .viewport_panel import ViewportPanel
from .media_info_bar import MediaInfoBar
from .controls_bar import ControlsBar
from .timeline_area import TimelineArea
from .track_row import TrackRow
from .theme_utils import get_color, get_color_hex, get_accent_color, get_accent_color_hex

# Core (需要 voidview_native)
# from .sync_controller import SyncController, VideoSource
# from .playback_manager import PlaybackManager, PlayState

__all__ = [
    # Logging
    'setup_logging',
    'get_logger',
    # UI
    'ViewMode',
    'MainWindow',
    'ToolBar',
    'ViewportPanel',
    'MediaInfoBar',
    'ControlsBar',
    'TimelineArea',
    'TrackRow',
    # Theme
    'get_color',
    'get_color_hex',
    'get_accent_color',
    'get_accent_color_hex',
    # Core (暂不导出)
    # 'SyncController',
    # 'VideoSource',
    # 'PlaybackManager',
    # 'PlayState',
]
