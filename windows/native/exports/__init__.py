"""VoidPlayer Video Renderer — D3D11VA Multi-track Video Renderer.

This package wraps the native video_renderer_native extension module,
providing typed, documented Python bindings.

Usage::

    from video_renderer import Renderer, RendererConfig, LogConfig

    # Optional: configure logging before creating a renderer
    log_cfg = LogConfig()
    log_cfg.level = 0  # trace
    configure_logging(log_cfg)

    config = RendererConfig()
    config.video_paths = ["video.mp4"]
    config.hwnd = int(window.winId())
    config.width = 1920
    config.height = 1080

    renderer = Renderer()
    renderer.initialize(config)
    renderer.play()

    print(f"Duration: {renderer.duration_us() / 1_000_000:.1f}s")
    print(f"Tracks: {renderer.track_count()}")

    # ... later ...
    renderer.shutdown()
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
