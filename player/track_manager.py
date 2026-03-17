"""
TrackManager - 媒体轨道数据管理器 (唯一数据源)
"""
from PySide6.QtCore import QObject, Signal


class TrackManager(QObject):
    """
    媒体轨道数据管理器 - 统一管理所有媒体源的增删改查

    作为唯一数据源，所有组件的媒体数据变更都通过此类进行，
    变更后通过信号通知相关组件更新显示。
    """

    # 信号
    source_added = Signal(int, str)      # index, source
    source_removed = Signal(int)         # index
    sources_swapped = Signal(int, int)   # index1, index2 (交换)
    sources_reordered = Signal(int, int) # old_index, new_index (移动)
    sources_reset = Signal()             # 全部重置

    def __init__(self, parent=None):
        super().__init__(parent)
        self._sources: list[str] = []

    # ========== 查询 ==========

    def sources(self) -> list[str]:
        """获取当前源列表 (副本)"""
        return self._sources.copy()

    def count(self) -> int:
        """获取源数量"""
        return len(self._sources)

    def get(self, index: int) -> str | None:
        """获取指定索引的源"""
        if 0 <= index < len(self._sources):
            return self._sources[index]
        return None

    # ========== 修改操作 ==========

    def set_sources(self, sources: list[str]):
        """设置源列表 (批量替换)"""
        self._sources = sources.copy()
        self.sources_reset.emit()

    def add_source(self, source: str) -> int:
        """添加源，返回索引"""
        index = len(self._sources)
        self._sources.append(source)
        self.source_added.emit(index, source)
        return index

    def remove_source(self, index: int) -> bool:
        """移除源"""
        if not (0 <= index < len(self._sources)):
            return False
        self._sources.pop(index)
        self.source_removed.emit(index)
        return True

    def swap_sources(self, index1: int, index2: int) -> bool:
        """
        交换两个源的位置

        用于 viewport comboBox 选择时的交换操作

        Args:
            index1: 第一个索引 (通常是槽位索引)
            index2: 第二个索引 (通常是选择的媒体索引)

        Returns:
            是否执行了交换
        """
        if index1 == index2:
            return False

        if not (0 <= index1 < len(self._sources)):
            return False

        if not (0 <= index2 < len(self._sources)):
            return False

        # 执行交换
        self._sources[index1], self._sources[index2] = \
            self._sources[index2], self._sources[index1]

        self.sources_swapped.emit(index1, index2)
        return True

    def move_source(self, old_index: int, new_index: int) -> bool:
        """
        移动源到新位置

        用于 timeline 拖拽重排序

        Args:
            old_index: 原索引
            new_index: 目标索引

        Returns:
            是否执行了移动
        """
        if old_index == new_index:
            return False

        if not (0 <= old_index < len(self._sources)):
            return False

        if not (0 <= new_index < len(self._sources)):
            return False

        # 执行移动
        item = self._sources.pop(old_index)
        self._sources.insert(new_index, item)

        self.sources_reordered.emit(old_index, new_index)
        return True

    def clear(self):
        """清空所有源"""
        self._sources.clear()
        self.sources_reset.emit()
