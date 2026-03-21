"""
ActionResolvers - 交互式参数解析器
"""
from typing import Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from player.ui.main_window import MainWindow


def resolve_file_picker(mw: "MainWindow") -> Optional[dict]:
    """
    解析文件路径参数 - 单文件选择器

    Returns:
        {"file_path": str} 或 None (用户取消)
    """
    from PySide6.QtWidgets import QFileDialog

    files, _ = QFileDialog.getOpenFileNames(mw, "选择媒体文件", "", "所有文件 (*.*)")
    if files:
        return {"file_path": files[0]}
    return None


def resolve_multi_file_picker(mw: "MainWindow") -> Optional[dict]:
    """
    解析文件路径参数 - 多文件选择器

    Returns:
        {"file_paths": list[str]} 或 None (用户取消)
    """
    from PySide6.QtWidgets import QFileDialog

    files, _ = QFileDialog.getOpenFileNames(mw, "选择媒体文件", "", "所有文件 (*.*)")
    if files:
        return {"file_paths": files}
    return None


def resolve_save_path(mw: "MainWindow") -> Optional[dict]:
    """
    解析保存路径参数

    Returns:
        {"save_path": str} 或 None (用户取消)
    """
    from PySide6.QtWidgets import QFileDialog

    path, _ = QFileDialog.getSaveFileName(mw, "保存截图", "", "PNG 图片 (*.png)")
    if path:
        return {"save_path": path}
    return None
