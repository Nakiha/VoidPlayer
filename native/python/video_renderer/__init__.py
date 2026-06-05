"""VoidPlayer native Python convenience package.

This package wraps the native video_renderer_native extension module for
developer demos and local tooling.
"""

from video_renderer_native import (
    LogConfig,
    RendererConfig,
    Renderer,
    configure_logging,
    install_crash_handler,
    remove_crash_handler,
)

__all__ = [
    "LogConfig",
    "RendererConfig",
    "Renderer",
    "configure_logging",
    "install_crash_handler",
    "remove_crash_handler",
]
