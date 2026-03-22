"""
TrackLayout - 多轨道布局管理

计算每个 track 的分割视图区域
"""
from dataclasses import dataclass
from enum import Enum
from typing import Optional

from PySide6.QtCore import QRectF, QSizeF, QPointF


class LayoutMode(Enum):
    """布局模式"""
    SIDE_BY_SIDE = "side_by_side"  # 并排模式：1/n 等分
    SPLIT = "split"                 # 分屏模式：可调分割比


@dataclass
class TrackRegion:
    """轨道区域信息"""
    index: int          # track 索引
    region: QRectF      # 分割视图区域（QOpenGLWidget 坐标系）


class TrackLayout:
    """多轨道布局管理

    计算每个 track 的分割视图区域

    并排模式 (SIDE_BY_SIDE):
    - 每个 track 的分割视图固定为 1/n 等分
    - 最多支持 8 个轨道

    分屏模式 (SPLIT):
    - 只有 2 个视图
    - 分割比例可调整（默认 1/3 : 2/3）
    """

    MAX_TRACKS = 8

    def __init__(self):
        self._mode = LayoutMode.SIDE_BY_SIDE
        self._split_ratio = 1/3  # 分屏模式下的分割比例 (左侧占比)
        self._tracks: list[TrackRegion] = []
        self._widget_size: QSizeF = QSizeF(0, 0)

    # --- 属性 ---

    @property
    def mode(self) -> LayoutMode:
        return self._mode

    @property
    def split_ratio(self) -> float:
        return self._split_ratio

    @property
    def track_count(self) -> int:
        return len(self._tracks)

    # --- 配置 ---

    def set_mode(self, mode: LayoutMode):
        """设置布局模式"""
        if self._mode != mode:
            self._mode = mode
            self._invalidate_layout()

    def set_split_ratio(self, ratio: float):
        """设置分屏模式的分割比例 (0.1 ~ 0.9)"""
        old_ratio = self._split_ratio
        self._split_ratio = max(0.1, min(0.9, ratio))
        if old_ratio != self._split_ratio:
            self._invalidate_layout()

    # --- 布局计算 ---

    def update_layout(self, widget_size: QSizeF, track_count: int):
        """更新布局

        Args:
            widget_size: QOpenGLWidget 的总大小
            track_count: track 数量
        """
        self._widget_size = widget_size
        track_count = min(track_count, self.MAX_TRACKS)

        self._tracks.clear()

        if track_count == 0 or widget_size.isEmpty():
            return

        w, h = widget_size.width(), widget_size.height()

        if self._mode == LayoutMode.SIDE_BY_SIDE:
            self._layout_side_by_side(track_count, w, h)
        elif self._mode == LayoutMode.SPLIT:
            self._layout_split(track_count, w, h)

    def _layout_side_by_side(self, track_count: int, w: float, h: float):
        """并排模式布局"""
        segment_width = w / track_count
        for i in range(track_count):
            region = QRectF(
                i * segment_width, 0,
                segment_width, h
            )
            self._tracks.append(TrackRegion(i, region))

    def _layout_split(self, track_count: int, w: float, h: float):
        """分屏模式布局"""
        if track_count == 1:
            # 单轨道时占满
            region = QRectF(0, 0, w, h)
            self._tracks.append(TrackRegion(0, region))
        else:
            # 多轨道时按分割比分配
            split_x = w * self._split_ratio
            self._tracks.append(TrackRegion(0, QRectF(
                0, 0, split_x, h
            )))
            self._tracks.append(TrackRegion(1, QRectF(
                split_x, 0, w - split_x, h
            )))
            # SPLIT 模式只支持 2 个视图，忽略多余的 track

    def _invalidate_layout(self):
        """使布局缓存失效，需要重新 update_layout"""
        # 触发外部调用 update_layout
        pass

    # --- 查询 ---

    def get_track_at(self, pos: QPointF) -> int:
        """获取指定位置所属的 track 索引

        Args:
            pos: QOpenGLWidget 坐标系中的位置

        Returns:
            track 索引，-1 表示不在任何 track 区域内

        Example:
            widget = 1920×1000, 2 tracks, 1/3 分割
            pos = (640, 500) → track 1 (属于右侧区域)
            pos = (320, 500) → track 0 (属于左侧区域)
        """
        for track in self._tracks:
            if track.region.contains(pos):
                return track.index
        return -1

    def get_viewport_region(self, track_index: int) -> Optional[QRectF]:
        """获取指定 track 的分割视图区域"""
        for track in self._tracks:
            if track.index == track_index:
                return track.region
        return None

    def get_all_regions(self) -> list[TrackRegion]:
        """获取所有 track 区域"""
        return self._tracks.copy()

    def get_widget_size(self) -> QSizeF:
        """获取 widget 大小"""
        return self._widget_size


def normalize_mouse_position(
    widget_pos: QPointF,
    track_region: QRectF,
) -> QPointF:
    """计算鼠标在分割视图中的归一化位置

    Args:
        widget_pos: QOpenGLWidget 坐标系中的鼠标位置
        track_region: track 的分割视图区域

    Returns:
        归一化位置 (0~1, 0~1)

    Example:
        widget = 1920×1000, 2 tracks, 1/3 分割
        track_1 region = (640, 0, 1280, 1000)
        mouse at (640, 500) in widget
        → normalized = ((640-640)/1280, (500-0)/1000) = (0.0, 0.5)
    """
    local_x = widget_pos.x() - track_region.left()
    local_y = widget_pos.y() - track_region.top()

    width = track_region.width()
    height = track_region.height()

    if width <= 0 or height <= 0:
        return QPointF(0.5, 0.5)

    norm_x = local_x / width
    norm_y = local_y / height

    # 钳制到 0~1 范围
    norm_x = max(0.0, min(1.0, norm_x))
    norm_y = max(0.0, min(1.0, norm_y))

    return QPointF(norm_x, norm_y)


def calculate_fit_ratio(track_size: QSizeF, viewport_size: QSizeF) -> float:
    """计算 fit 缩放比例

    Args:
        track_size: track 的片分辨率
        viewport_size: 分割视图的大小

    Returns:
        使画面填满最短边所需的缩放比例

    Example:
        track = 1920x1080, viewport = 960x1000
        fit_w = 960 / 1920 = 0.5   # 按宽度缩放
        fit_h = 1000 / 1080 = 0.926  # 按高度缩放
        取较大值以填满最短边: max(0.5, 0.926) = 0.926
    """
    if track_size.isEmpty() or viewport_size.isEmpty():
        return 1.0

    fit_w = viewport_size.width() / track_size.width()
    fit_h = viewport_size.height() / track_size.height()
    return max(fit_w, fit_h)
