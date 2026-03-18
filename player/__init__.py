"""
VoidPlayer - Multi-video playback module
"""

# Logging
from .core.logging_config import setup_logging, get_logger

# UI 组件
from .ui.viewport import ViewMode, ViewportPanel, MediaHeader
from .ui.main_window import MainWindow
from .ui.toolbar import ToolBar
from .ui.controls_bar import ControlsBar
from .ui.timeline_area import TimelineArea
from .ui.track_row import TrackRow
from .theme_utils import get_color, get_color_hex, get_accent_color, get_accent_color_hex, ColorKey

__all__ = [
    # Logging
    'setup_logging',
    'get_logger',
    # UI
    'ViewMode',
    'MainWindow',
    'ToolBar',
    'ViewportPanel',
    'MediaHeader',
    'ControlsBar',
    'TimelineArea',
    'TrackRow',
    # Theme
    'get_color',
    'get_color_hex',
    'get_accent_color',
    'get_accent_color_hex',
    'ColorKey',
]
